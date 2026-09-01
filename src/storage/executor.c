#define _GNU_SOURCE   // Enables ALL modern POSIX/Linux features
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>    // For SHOW DATABASES / TABLES
#include "pager.h"
#include "btree.h"
#include "schema.h"
#include "../parser/parser.h" 
#include <sys/stat.h>  // For mkdir() and stat()
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>

extern int is_recovery_mode; // Link to the flag in server.c

// =========================================================
// TABLE-LEVEL CONCURRENCY CONTROL (Lock Manager)
// =========================================================
typedef struct {
    char table_name[256];
    pthread_rwlock_t rwlock;
} TableLock;

static TableLock table_locks[100];
static int num_table_locks = 0;
static pthread_mutex_t lock_manager_mutex = PTHREAD_MUTEX_INITIALIZER;

// Dynamically gets or creates a Read-Write lock for a specific table.
// Returns NULL if table_name doesn't fit the lock slot or the table
// count exceeds table_locks' fixed capacity — callers must check.
pthread_rwlock_t* get_table_lock(const char* table_name) {
    if (strlen(table_name) >= sizeof(table_locks[0].table_name)) {
        return NULL;
    }

    pthread_mutex_lock(&lock_manager_mutex);

    // Check if lock already exists
    for(int i = 0; i < num_table_locks; i++) {
        if(strcasecmp(table_locks[i].table_name, table_name) == 0) {
            pthread_mutex_unlock(&lock_manager_mutex);
            return &table_locks[i].rwlock;
        }
    }

    if (num_table_locks >= (int)(sizeof(table_locks) / sizeof(table_locks[0]))) {
        pthread_mutex_unlock(&lock_manager_mutex);
        return NULL;
    }

    // Create new lock for this table
    strcpy(table_locks[num_table_locks].table_name, table_name);
    pthread_rwlock_init(&table_locks[num_table_locks].rwlock, NULL);
    pthread_rwlock_t* new_lock = &table_locks[num_table_locks].rwlock;
    num_table_locks++;

    pthread_mutex_unlock(&lock_manager_mutex);
    return new_lock;
}

extern void trim_string(char *str);

// =========================================================
// THE SANDBOX & SYSTEM HELPERS
// =========================================================
#define DATA_DIR "flexql_data"

// Ensures the master data directory exists
void ensure_sandbox() {
    struct stat st = {0};
    if (stat(DATA_DIR, &st) == -1) mkdir(DATA_DIR, 0777);
}

// Creates a new directory for the database
int execute_create_db(ParsedQuery* query) {
    ensure_sandbox();
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, query->db_name);
    
    if (mkdir(filepath, 0777) == 0) {
        printf("[+] Database '%s' created successfully.\n", query->db_name);
        return 0;
    } else {
        return -1;
    }
}

// Verifies the database exists and updates the client's session string
int execute_use_db(ParsedQuery* query, char* current_db_session) {
    ensure_sandbox();
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, query->db_name);
    
    struct stat st = {0};
    if (stat(filepath, &st) == -1) {
        return -1;
    }
    
    strcpy(current_db_session, query->db_name);
    return 0;
}

int execute_drop_db(ParsedQuery* query, char* current_db_session) {
    char cmd[1024]; char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, query->db_name);

    struct stat st = {0};
    if (stat(filepath, &st) == -1) return -1; // Doesn't exist!

    if (strcmp(query->db_name, "default_db") == 0) snprintf(cmd, sizeof(cmd), "rm -rf %s/*", filepath);
    else snprintf(cmd, sizeof(cmd), "rm -rf %s", filepath);
    
    system(cmd);
    if (strcmp(current_db_session, query->db_name) == 0) current_db_session[0] = '\0'; 
    return 0;
}

int execute_drop_table(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) return -1;
    char filepath[512];

    // Grab EXCLUSIVE lock so a concurrent reader/writer can't be mid-scan
    // or mid-insert on the file while we unlink it out from under them.
    pthread_rwlock_t* t_lock = get_table_lock(query->table_name);
    if (t_lock == NULL) return -1;
    pthread_rwlock_wrlock(t_lock);

    snprintf(filepath, sizeof(filepath), "%s/%s/%s.schema", DATA_DIR, current_db, query->table_name);
    if (remove(filepath) != 0) {
        pthread_rwlock_unlock(t_lock);
        return -1; // If schema isn't there, table doesn't exist!
    }

    snprintf(filepath, sizeof(filepath), "%s/%s/%s.dat", DATA_DIR, current_db, query->table_name);
    remove(filepath);
    // The shared Pager for this table is keyed by this exact path. If we
    // leave its registry entry in place, a table later re-created at the
    // same path would silently reuse a Pager pointing at the deleted file.
    pager_retire(filepath);

    char db_path[512]; snprintf(db_path, sizeof(db_path), "%s/%s", DATA_DIR, current_db);
    delete_root_page(db_path, query->table_name);

    pthread_rwlock_unlock(t_lock);
    return 0;
}

void execute_show_db(int client_sock) {
    ensure_sandbox();
    DIR *d; struct dirent *dir;
    char buffer[4096] = "";
    d = opendir(DATA_DIR);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            // --- Ignore hidden files, ., .., and the WAL file ---
            if (dir->d_name[0] == '.') continue; 
            if (strstr(dir->d_name, "recovery.wal") != NULL) continue;
            if (dir->d_type == DT_DIR && strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
                char row[512];
                snprintf(row, sizeof(row), "ROW|1|Database|%s\n", dir->d_name);
                strcat(buffer, row);
            }
        }
        closedir(d);
    }
    strcat(buffer, "DONE|Query executed successfully.\n");
    send(client_sock, buffer, strlen(buffer), 0);
}

void execute_show_tables(const char* current_db, int client_sock) {
    if (strlen(current_db) == 0) {
        send(client_sock, "ERROR|No database selected.\n", 28, 0); return;
    }
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", DATA_DIR, current_db);
    
    DIR *d; struct dirent *dir;
    char buffer[4096] = "";
    d = opendir(filepath);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            // --- Ignore hidden files, ., .., and the WAL file ---
            if (dir->d_name[0] == '.') continue; 
            if (strstr(dir->d_name, "recovery.wal") != NULL) continue;

            if (strstr(dir->d_name, ".schema")) {
                char row[512]; char t_name[512];
                strcpy(t_name, dir->d_name);
                t_name[strlen(t_name) - 7] = '\0'; // Remove .schema
                snprintf(row, sizeof(row), "ROW|1|Table|%s\n", t_name);
                strcat(buffer, row);
            }
        }
        closedir(d);
    }
    strcat(buffer, "DONE|Query executed successfully.\n");
    send(client_sock, buffer, strlen(buffer), 0);
}



// =========================================================
// TYPE SAFETY ENGINE
// =========================================================

int safe_parse_int(const char* str, int32_t* out_val) {
    char* endptr;
    *out_val = (int32_t)strtol(str, &endptr, 10);
    return (*str != '\0' && *endptr == '\0');
}

int safe_parse_double(const char* str, double* out_val) {
    char* endptr;
    *out_val = strtod(str, &endptr);
    return (*str != '\0' && *endptr == '\0');
}



// --- 1. Your Excellent Serialization Logic ---
// Change the signature to take `int* cached_types` instead of `ColumnDef* schema`
// Returns -1 on type violation
int serialize_row(ParsedQuery* query, int* cached_types, char* tuple_buffer) {
    uint16_t offset = 0;
    TupleHeader header;
    header.expiration_timestamp = (uint64_t)time(NULL) + (30 * 24 * 60 * 60); 
    memcpy(tuple_buffer + offset, &header, sizeof(TupleHeader));
    offset += sizeof(TupleHeader);

    for (int i = 0; i < query->value_count; i++) {
        // Handle NULLs / Empties
        if (strcasecmp(query->values[i], "NULL") == 0 || strlen(query->values[i]) == 0) {
             // For simplicity, we write 0s for missing data unless we fully build a null-bitmap
             int32_t zero = 0;
             if (cached_types[i] == 1) { memcpy(tuple_buffer+offset, &zero, 4); offset+=4; }
             else if (cached_types[i] == 2 || cached_types[i] == 3) { double dz=0; memcpy(tuple_buffer+offset, &dz, 8); offset+=8; }
             else { uint16_t l=0; memcpy(tuple_buffer+offset, &l, 2); offset+=2; }
             continue;
        }

        if (cached_types[i] == 1) { // INT
            int32_t val;
            if (!safe_parse_int(query->values[i], &val)) return -1;
            memcpy(tuple_buffer + offset, &val, 4); offset += 4;
        } else if (cached_types[i] == 2) { // DECIMAL 
            double val;
            if (!safe_parse_double(query->values[i], &val)) return -1;
            memcpy(tuple_buffer + offset, &val, 8); offset += 8;
        } else if (cached_types[i] == 3) { // DATETIME
            struct tm tm_info; memset(&tm_info, 0, sizeof(struct tm));
            int64_t timestamp = 0;
            if (strptime(query->values[i], "%Y-%m-%d %H:%M:%S", &tm_info) != NULL ||
                strptime(query->values[i], "%Y-%m-%d", &tm_info) != NULL) {
                timestamp = (int64_t)mktime(&tm_info);
            } else {
                // THE FIX: Mathematically prove the string is purely an integer before allowing atoll!
                int is_num = 1;
                for (int k = 0; query->values[i][k] != '\0'; k++) {
                    if (query->values[i][k] < '0' || query->values[i][k] > '9') { is_num = 0; break; }
                }
                if (is_num && strlen(query->values[i]) > 0) {
                    timestamp = atoll(query->values[i]); 
                } else {
                    return -1; // Bad Date Format!
                }
            }
            memcpy(tuple_buffer + offset, &timestamp, 8); offset += 8;
        } else { // VARCHAR
            uint16_t len = strlen(query->values[i]);
            memcpy(tuple_buffer + offset, &len, 2); offset += 2;
            memcpy(tuple_buffer + offset, query->values[i], len); offset += len;
        }
    }
    return offset; 
}

int insert_into_page(Page* page, const char* tuple_buffer, uint16_t tuple_size) {
    uint16_t slots_end_offset = sizeof(PageHeader) + (page->header.num_slots * sizeof(Slot));
    uint16_t free_space = page->header.free_space_ptr - slots_end_offset;

    if (free_space < (tuple_size + sizeof(Slot))) return -1; // PAGE IS FULL

    page->header.free_space_ptr -= tuple_size;
    uint16_t data_array_index = page->header.free_space_ptr - sizeof(PageHeader);
    memcpy(&page->data[data_array_index], tuple_buffer, tuple_size);

    Slot* slot_array = (Slot*)page->data; 
    slot_array[page->header.num_slots].offset = page->header.free_space_ptr;
    slot_array[page->header.num_slots].length = tuple_size;

    page->header.num_slots++;
    return 0; 
}

// --- The Thread-Safe Pager Wrapper ---
RecordID append_to_data_page(Pager* pager, uint32_t* active_data_page, const char* tuple_buffer, uint16_t tuple_size) {
    
    Page* page = get_page(pager, *active_data_page);

    // SAFETY NET: If this page is brand new, its free_space_ptr is 0. We MUST initialize it!
    if (page->header.free_space_ptr == 0) {
        page->header.page_type = 0;
        page->header.num_slots = 0;
        page->header.free_space_ptr = 4096;
    }

    // If it's an index page, or it's full...
    if (page->header.page_type != 0 || insert_into_page(page, tuple_buffer, tuple_size) == -1) {
        unpin_page(pager, *active_data_page, 0); 
        
        // Jump to the absolute end of the file to guarantee we don't hit a B-Tree node
        *active_data_page = pager->num_pages; 
        page = get_page(pager, *active_data_page);
        
        page->header.page_type = 0; 
        page->header.num_slots = 0;
        page->header.free_space_ptr = 4096; 
        
        insert_into_page(page, tuple_buffer, tuple_size); 
    }

    uint16_t slot_num = page->header.num_slots - 1; 
    unpin_page(pager, *active_data_page, 1); // Mark dirty to save to disk

    RecordID rec = { .page_num = *active_data_page, .slot_num = slot_num };
    return rec;
}
// =========================================================
//                   THE MAIN EXECUTOR API
// =========================================================

int execute_create(const char* current_db, ParsedQuery* query) {
    ensure_sandbox();
    if (strlen(current_db) == 0) return -1;

    char path[1024]; // Increased size to silence GCC warning
    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, current_db);
    mkdir(path, 0777); 
    
    char filepath[1024]; // Increased size to silence GCC warning
    snprintf(filepath, sizeof(filepath), "%s/%s.dat", path, query->table_name);
    
    // --- THE COMBINED OVERWRITE & RECOVERY FIX ---
    // Check if the table's .dat file already exists using POSIX access()
    if (access(filepath, F_OK) == 0) {
        if (is_recovery_mode) {
            // It survived the crash! Skip wiping it and return success for the WAL.
            return 0; 
        } else {
            // A live client is trying to overwrite an existing table. Abort!
            // Return -2 to signal a "Table Already Exists" conflict to the caller.
            return -2; 
        }
    }
    // ---------------------------------------------

    if (save_schema(path, query->table_name, query->columns, query->column_count) == 0) {
        Pager* pager = pager_open(filepath);
        if (pager == NULL) return -1;
        Page* root_page = get_page(pager, 0);
        root_page->header.page_type = 1;
        BTreeNode* root_node = (BTreeNode*)root_page->data;
        root_node->is_leaf = 1; root_node->is_root = 1; root_node->num_keys = 0;
        unpin_page(pager, 0, 1);
        // Make the root page durable now, not whenever LRU eviction next
        // happens to run. Without this, the file existing on disk doesn't
        // mean its root page does - see pager_flush_all()'s comment.
        pager_flush_all(pager);
        return 0; // SUCCESS
    }
    return -1; // FAILURE
}

int execute_delete(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) return -1;
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s/%s.dat", DATA_DIR, current_db, query->table_name);

    // Grab EXCLUSIVE lock — same reason as execute_drop_table: this unlinks
    // and recreates the .dat file, which must not race a reader's scan or
    // another writer's insert.
    pthread_rwlock_t* t_lock = get_table_lock(query->table_name);
    if (t_lock == NULL) return -1;
    pthread_rwlock_wrlock(t_lock);

    struct stat st = {0};
    if (stat(filepath, &st) == -1) {
        pthread_rwlock_unlock(t_lock);
        return -1; // File missing!
    }

    remove(filepath);
    // Retire the OLD shared Pager for this exact path before reopening it -
    // otherwise pager_open() below would find the stale registry entry and
    // hand back a Pager still pointing at the file we just unlinked.
    pager_retire(filepath);
    Pager* pager = pager_open(filepath);
    if (pager == NULL) { pthread_rwlock_unlock(t_lock); return -1; }
    Page* root_page = get_page(pager, 0);
    root_page->header.page_type = 1;
    BTreeNode* root_node = (BTreeNode*)root_page->data;
    root_node->is_leaf = 1; root_node->is_root = 1; root_node->num_keys = 0;
    unpin_page(pager, 0, 1);
    pager_flush_all(pager); // Same reason as execute_create() - make it durable now.

    char db_path[512]; snprintf(db_path, sizeof(db_path), "%s/%s", DATA_DIR, current_db);
    delete_root_page(db_path, query->table_name); // Fresh table, root is page 0 again.

    pthread_rwlock_unlock(t_lock);
    return 0;
}


int execute_insert(const char* current_db, Pager* pager, uint32_t* root_page_id, uint32_t* active_data_page, ParsedQuery* query, int client_sock) {
    if (strlen(current_db) == 0) {
        send(client_sock, "ERROR|No database selected.\n", 28, 0); return -1;
    }
    
    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s", DATA_DIR, current_db);

    static __thread char cached_table[256] = "";
    static __thread ColumnDef cached_schema[100];
    static __thread int cached_num_cols = 0;
    static __thread int cached_types[100]; 

    if (strcmp(cached_table, query->table_name) != 0) {
        cached_num_cols = load_schema(db_path, query->table_name, cached_schema);
        if (cached_num_cols != -1) {
            strcpy(cached_table, query->table_name);
            for (int i = 0; i < cached_num_cols; i++) {
                trim_string(cached_schema[i].name); trim_string(cached_schema[i].type);
                if (strcasecmp(cached_schema[i].type, "INT") == 0) cached_types[i] = 1;
                else if (strcasecmp(cached_schema[i].type, "DECIMAL") == 0) cached_types[i] = 2;
                else if (strcasecmp(cached_schema[i].type, "DATETIME") == 0) cached_types[i] = 3;
                else cached_types[i] = 4; 
            }
        } else {
            cached_table[0] = '\0';
        }
    }

    if (cached_num_cols == -1) {
        const char* err = "ERROR|Table does not exist.\n";
        send(client_sock, err, strlen(err), 0);
        // bulk_insert_ptr was already strdup'd by the parser before we ever
        // got here - valgrind caught this as a genuine small leak (INSERT
        // INTO a table that doesn't exist), one per rejected statement.
        free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
        return -1;
    }

    if (query->bulk_insert_ptr == NULL) return -1;

    char* ptr = query->bulk_insert_ptr;
    char tuple_buffer[MAX_TUPLE_SIZE];

    // Whether this table even has a B+Tree worth tracking a root for - skip
    // all the sidecar I/O below for heap-only tables (e.g. no-PK benchmark
    // tables), where root_page_id is meaningless anyway.
    int table_has_pk = 0;
    for (int i = 0; i < cached_num_cols; i++) {
        if (cached_schema[i].is_primary_key) { table_has_pk = 1; break; }
    }

    // Grab EXCLUSIVE Write Lock!
    pthread_rwlock_t* t_lock = get_table_lock(query->table_name);
    if (t_lock == NULL) {
        const char* err = "ERROR|Table name too long or too many distinct tables locked this session.\n";
        send(client_sock, err, strlen(err), 0);
        return -1;
    }
    pthread_rwlock_wrlock(t_lock);

    // Re-read the root fresh now that we hold the exclusive lock. The value
    // the caller passed in may have been cached since this connection last
    // attached to this table, and another connection could have split the
    // tree - and saved a newer root - any time since. Every exit below
    // saves it back before unlocking, so the next lock holder (this
    // connection or another) always sees a state that's actually current.
    if (table_has_pk) *root_page_id = load_root_page(db_path, query->table_name);

    while (*ptr != '\0') {
        if (*ptr == '(') {
            ptr++;
            query->value_count = 0;
            char val_buf[512]; int v_idx = 0;

            while (*ptr != ')' && *ptr != '\0') {
                if (*ptr == ',') {
                    val_buf[v_idx] = '\0'; trim_string(val_buf);
                    strcpy(query->values[query->value_count++], val_buf);
                    v_idx = 0;
                } else {
                    val_buf[v_idx++] = *ptr;
                }
                ptr++;
            }
            if (v_idx > 0) {
                val_buf[v_idx] = '\0'; trim_string(val_buf);
                strcpy(query->values[query->value_count++], val_buf);
            }

            // SEMANTIC CONSTRAINT: Column Count Match
            if (query->value_count != cached_num_cols) {
                char err[128]; snprintf(err, 128, "ERROR|Column count mismatch. Expected %d, got %d.\n", cached_num_cols, query->value_count);
                send(client_sock, err, strlen(err), 0);
                free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
                if (table_has_pk) save_root_page(db_path, query->table_name, *root_page_id);
                pager_flush_all(pager); // Same statement, same durability granularity as the WAL fflush.
                pthread_rwlock_unlock(t_lock); // <--- Unlock the lock
                return -1;
            }

            // SEMANTIC CONSTRAINT: NOT NULL Check
            for (int i = 0; i < cached_num_cols; i++) {
                if (cached_schema[i].is_not_null || cached_schema[i].is_primary_key) {
                    if (strlen(query->values[i]) == 0 || strcasecmp(query->values[i], "NULL") == 0) {
                        char err[128]; snprintf(err, 128, "ERROR|NOT NULL constraint violation on column '%s'.\n", cached_schema[i].name);
                        send(client_sock, err, strlen(err), 0);
                        free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
                        if (table_has_pk) save_root_page(db_path, query->table_name, *root_page_id);
                        pager_flush_all(pager); // Same statement, same durability granularity as the WAL fflush.
                        pthread_rwlock_unlock(t_lock); // <--- Unlock the lock
                        return -1;
                    }
                }
            }

            // =========================================================
            // DYNAMIC PK LOOKUP & HEAP TABLE DETECTION
            // =========================================================
            int pk_idx = 0; 
            int has_pk = 0; 
            for (int i = 0; i < cached_num_cols; i++) {
                if (cached_schema[i].is_primary_key) { 
                    pk_idx = i; 
                    has_pk = 1; 
                    break; 
                }
            }

            // IndexKey's value union is 32 bytes (str_val), but an INT or
            // DECIMAL key only ever writes 4 or 8 of them below - the whole
            // struct still gets copied into the B+Tree leaf's keys[] array
            // and flushed to disk as-is, so the unused union bytes were
            // raw stack garbage. valgrind flagged it; zero the struct once
            // here instead of chasing it at every write site downstream.
            IndexKey pk = {0};
            if (has_pk) {
                // Extract Primary Key for validation
                if (cached_types[pk_idx] == 1) {
                    if (!safe_parse_int(query->values[pk_idx], &pk.value.int_val)) goto type_err;
                    pk.type = 1;
                } else if (cached_types[pk_idx] == 2) {
                    if (!safe_parse_double(query->values[pk_idx], &pk.value.dec_val)) goto type_err;
                    pk.type = 2;
                } else {
                    pk.type = 4; strncpy(pk.value.str_val, query->values[pk_idx], 32);
                }

                // SEMANTIC CONSTRAINT: Duplicate Key Check! (ONLY IF WE HAVE A PK)
                RecordID dummy;
                if (btree_search(pager, *root_page_id, pk, &dummy)) {

                    // --- IDEMPOTENT REPLAY ---
                    // If we are booting up, and the B-Tree already has this ID, 
                    // it means this row survived the crash! Skip it safely!
                    if (is_recovery_mode) {
                        continue; 
                    }
                    // -------------------------

                    const char* err = "ERROR|Duplicate Primary Key constraint violation.\n";
                    send(client_sock, err, strlen(err), 0);
                    free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
                    if (table_has_pk) save_root_page(db_path, query->table_name, *root_page_id);
                    pager_flush_all(pager); // Same statement, same durability granularity as the WAL fflush.
                    pthread_rwlock_unlock(t_lock); // <--- Unlock the lock
                    return -1;
                }
            }

            // SEMANTIC CONSTRAINT: Type Safety Check!
            int tuple_size = serialize_row(query, cached_types, tuple_buffer);
            if (tuple_size == -1) {
type_err:
                const char* err = "ERROR|Invalid datatype or missing required field.\n";
                send(client_sock, err, strlen(err), 0);
                free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
                if (table_has_pk) save_root_page(db_path, query->table_name, *root_page_id);
                pager_flush_all(pager); // Same statement, same durability granularity as the WAL fflush.
                pthread_rwlock_unlock(t_lock); // <--- Unlock the lock
                return -1;
            }

            // ALWAYS save to the hard drive (Data Page)
            RecordID record_location = append_to_data_page(pager, active_data_page, tuple_buffer, tuple_size);
            
            // ONLY save to the Index (B-Tree) if the table has a Primary Key!
            if (has_pk) {
                btree_insert(pager, root_page_id, pk, record_location);
            }
        }
        ptr++;
    }

    free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
    if (table_has_pk) save_root_page(db_path, query->table_name, *root_page_id);
    pager_flush_all(pager); // Same statement, same durability granularity as the WAL fflush.
    pthread_rwlock_unlock(t_lock); // Release lock!
    return 0;
}


// --- HELPER STRUCTURES FOR TABLE SCANS & SORTING ---
typedef struct {
    char* row_str;
    double sort_num;
    char sort_str[64];
} SortRow;

int g_sort_type = 0; // 1 for Numbers, 2 for Strings

int compare_sort_rows(const void* a, const void* b) {
    SortRow* ra = (SortRow*)a;
    SortRow* rb = (SortRow*)b;
    if (g_sort_type == 1) {
        if (ra->sort_num < rb->sort_num) return -1;
        if (ra->sort_num > rb->sort_num) return 1;
        return 0;
    } else {
        return strcmp(ra->sort_str, rb->sort_str);
    }
}

// Strips table prefixes (e.g., "TEST_USERS.NAME" -> "NAME")
const char* get_base_col(const char* full_col) {
    const char* dot = strchr(full_col, '.');
    return dot ? dot + 1 : full_col;
}

int evaluate_condition(int type, char* record_ptr, const char* op, const char* val_str) {
    if (type == 1) { // INT
        int32_t val; memcpy(&val, record_ptr, 4);
        int32_t cmp = atoi(val_str);
        if (strcmp(op, "=") == 0) return val == cmp;
        if (strcmp(op, ">") == 0) return val > cmp;
        if (strcmp(op, "<") == 0) return val < cmp;
        if (strcmp(op, ">=") == 0) return val >= cmp;
        if (strcmp(op, "<=") == 0) return val <= cmp;
    } else if (type == 2) { // DECIMAL
        double val; memcpy(&val, record_ptr, 8);
        double cmp = atof(val_str);
        if (strcmp(op, "=") == 0) return val == cmp;
        if (strcmp(op, ">") == 0) return val > cmp;
        if (strcmp(op, "<") == 0) return val < cmp;
        if (strcmp(op, ">=") == 0) return val >= cmp;
        if (strcmp(op, "<=") == 0) return val <= cmp;
    } else if (type == 3) { /// DATETIME
        int64_t val; memcpy(&val, record_ptr, 8);
        int64_t cmp = 0;
        
        // Convert the WHERE string into a true Epoch integer!
        struct tm tm_info; memset(&tm_info, 0, sizeof(struct tm));
        if (strptime(val_str, "%Y-%m-%d %H:%M:%S", &tm_info) != NULL ||
            strptime(val_str, "%Y-%m-%d", &tm_info) != NULL) {
            cmp = (int64_t)mktime(&tm_info);
        } else {
            cmp = atoll(val_str); // Fallback
        }

        if (strcmp(op, "=") == 0) return val == cmp;
        if (strcmp(op, ">") == 0) return val > cmp;
        if (strcmp(op, "<") == 0) return val < cmp;
        if (strcmp(op, ">=") == 0) return val >= cmp;
        if (strcmp(op, "<=") == 0) return val <= cmp;
    } else { // VARCHAR
        uint16_t len; memcpy(&len, record_ptr, 2);
        char str_val[256] = {0};
        int copy_len = len < 255 ? len : 255;
        memcpy(str_val, record_ptr + 2, copy_len);
        int cmp = strcasecmp(str_val, val_str);
        if (strcmp(op, "=") == 0) return cmp == 0;
        if (strcmp(op, ">") == 0) return cmp > 0;
        if (strcmp(op, "<") == 0) return cmp < 0;
        if (strcmp(op, ">=") == 0) return cmp >= 0;
        if (strcmp(op, "<=") == 0) return cmp <= 0;
    }
    return 0;
}

// Releases the two join-side locks acquired by execute_select. b is NULL
// for a self-join (only a was ever locked), so it must be skipped rather
// than unlocked a second time.
void unlock_join_locks(pthread_rwlock_t* a, pthread_rwlock_t* b) {
    if (b != NULL) pthread_rwlock_unlock(b);
    if (a != NULL) pthread_rwlock_unlock(a);
}

// True if a SELECT list entry (optionally "table.column") refers to
// column_name on the table named side_table. An unqualified entry matches
// by name alone; the caller is responsible for treating that as ambiguous
// if it also matches the other side.
int join_col_matches_side(const char* select_entry, const char* side_table, const char* column_name) {
    const char* dot = strchr(select_entry, '.');
    if (dot != NULL) {
        size_t qualifier_len = dot - select_entry;
        if (strlen(side_table) != qualifier_len || strncasecmp(select_entry, side_table, qualifier_len) != 0) {
            return 0;
        }
        return strcasecmp(dot + 1, column_name) == 0;
    }
    return strcasecmp(select_entry, column_name) == 0;
}

// Serializes one table's columns into a join output row, skipping columns
// selected[i] marks as unselected. Skipped columns are still walked over
// in the record so later columns keep decoding at the right offset.
char* emit_join_columns(char* net_ptr, const char* rec, ColumnDef* schema, int ncols, const int* selected) {
    uint16_t off = sizeof(TupleHeader);
    for (int i = 0; i < ncols; i++) {
        if (selected[i]) {
            *net_ptr++ = '|';
            int n_len = strlen(schema[i].name);
            memcpy(net_ptr, schema[i].name, n_len); net_ptr += n_len;
            *net_ptr++ = '|';
        }
        if (strcasecmp(schema[i].type, "INT") == 0) {
            int32_t v; memcpy(&v, rec + off, 4);
            if (selected[i]) net_ptr += sprintf(net_ptr, "%d", v);
            off += 4;
        } else if (strcasecmp(schema[i].type, "DECIMAL") == 0) {
            double v; memcpy(&v, rec + off, 8);
            if (selected[i]) net_ptr += sprintf(net_ptr, "%g", v);
            off += 8;
        } else if (strcasecmp(schema[i].type, "DATETIME") == 0) {
            int64_t v; memcpy(&v, rec + off, 8);
            if (selected[i]) {
                time_t t = (time_t)v;
                struct tm *tm_info = localtime(&t);
                char time_str[32];
                strftime(time_str, 32, "%Y-%m-%d %H:%M:%S", tm_info);
                net_ptr += sprintf(net_ptr, "%s", time_str);
            }
            off += 8;
        } else {
            uint16_t l; memcpy(&l, rec + off, 2);
            if (selected[i]) { memcpy(net_ptr, rec + off + 2, l); net_ptr += l; }
            off += 2 + l;
        }
    }
    return net_ptr;
}

void execute_select(const char* current_db, Pager* pager, uint32_t root_page_id, ParsedQuery* query, int client_sock) {
    char net_buffer[65536];  net_buffer[0] = '\0';
    if (strlen(current_db) == 0) {
        send(client_sock, "ERROR|No database selected.\n", 28, 0); return;
    }

    // Grab locks in a canonical order (by table name) rather than query
    // order, so a mirrored "A JOIN B" / "B JOIN A" pair can never form an
    // ABBA deadlock against each other. A self-join acquires only once,
    // since a second rdlock on the same table can deadlock against a
    // writer queued in between the two calls.
    pthread_rwlock_t* t_lock = NULL;
    pthread_rwlock_t* t_lock_B = NULL;

    if (query->has_join && strcasecmp(query->table_name, query->join_table) != 0) {
        pthread_rwlock_t* lock_left = get_table_lock(query->table_name);
        pthread_rwlock_t* lock_right = get_table_lock(query->join_table);
        if (lock_left == NULL || lock_right == NULL) {
            send(client_sock, "ERROR|Table name too long or too many distinct tables locked this session.\n", 78, 0);
            return;
        }
        if (strcasecmp(query->table_name, query->join_table) < 0) {
            pthread_rwlock_rdlock(lock_left);
            pthread_rwlock_rdlock(lock_right);
        } else {
            pthread_rwlock_rdlock(lock_right);
            pthread_rwlock_rdlock(lock_left);
        }
        t_lock = lock_left;
        t_lock_B = lock_right;
    } else {
        t_lock = get_table_lock(query->table_name);
        if (t_lock == NULL) {
            send(client_sock, "ERROR|Table name too long or too many distinct tables locked this session.\n", 78, 0);
            return;
        }
        pthread_rwlock_rdlock(t_lock);
    }

    // Re-read the primary table's root page id now that we actually hold
    // the lock, instead of trusting whatever the connection had cached from
    // an earlier attach. A concurrent writer on another connection could
    // have split the tree (and moved the root) since we last looked - a
    // read lock only guarantees no writer is active *right now*, not that
    // our own stale in-memory copy is still correct.
    {
        char root_db_path[512];
        snprintf(root_db_path, sizeof(root_db_path), "%s/%s", DATA_DIR, current_db);
        root_page_id = load_root_page(root_db_path, query->table_name);
    }

    // =========================================================
    // THE ULTIMATE INNER JOIN ENGINE (With WHERE & Fallbacks)
    // =========================================================
    if (query->has_join) {

        char db_path[512]; snprintf(db_path, sizeof(db_path), "%s/%s", DATA_DIR, current_db);

        // 1. Load Schemas
        ColumnDef schema_A[100]; int cols_A = load_schema(db_path, query->table_name, schema_A);
        if (cols_A == -1) { 
            send(client_sock, "ERROR|Left table missing.\n", 26, 0); 
            unlock_join_locks(t_lock, t_lock_B); return;
        }

        ColumnDef schema_B[100]; int cols_B = load_schema(db_path, query->join_table, schema_B);
        if (cols_B == -1) { 
            send(client_sock, "ERROR|Right table missing.\n", 27, 0); 
            unlock_join_locks(t_lock, t_lock_B); return;
        }

        // 2. Identify Join Columns
        int join_idx_A = -1, join_idx_B = -1;
        const char* base_left = get_base_col(query->join_condition_left);
        const char* base_right = get_base_col(query->join_condition_right);
        
        for (int i = 0; i < cols_A; i++) if (strcasecmp(schema_A[i].name, base_left) == 0 || strcasecmp(schema_A[i].name, base_right) == 0) join_idx_A = i;
        for (int i = 0; i < cols_B; i++) if (strcasecmp(schema_B[i].name, base_left) == 0 || strcasecmp(schema_B[i].name, base_right) == 0) join_idx_B = i;

        if (join_idx_A == -1 || join_idx_B == -1) { 
            send(client_sock, "ERROR|Join columns not found.\n", 30, 0); 
            unlock_join_locks(t_lock, t_lock_B); return;
        }
        
        int type_join = 4;
        if (strcasecmp(schema_A[join_idx_A].type, "INT") == 0) type_join = 1;
        else if (strcasecmp(schema_A[join_idx_A].type, "DECIMAL") == 0) type_join = 2;
        else if (strcasecmp(schema_A[join_idx_A].type, "DATETIME") == 0) type_join = 3;

        // 3. Find the TRUE Primary Key indices in RAM
        int pk_idx_A = -1, pk_idx_B = -1;
        for (int i = 0; i < cols_A; i++) if (schema_A[i].is_primary_key) pk_idx_A = i;
        for (int i = 0; i < cols_B; i++) if (schema_B[i].is_primary_key) pk_idx_B = i;

        // 4. PREPARE THE WHERE CLAUSE FILTER (If applicable)
        int has_where_A = 0, has_where_B = 0;
        int where_idx = -1;
        int where_type = 4;
        
        if (query->has_where) {
            const char* w_col = get_base_col(query->where_column);
            // Check if WHERE applies to Table A
            for (int i = 0; i < cols_A; i++) {
                if (strcasecmp(schema_A[i].name, w_col) == 0) {
                    has_where_A = 1; where_idx = i;
                    if (strcasecmp(schema_A[i].type, "INT") == 0) where_type = 1;
                    else if (strcasecmp(schema_A[i].type, "DECIMAL") == 0) where_type = 2;
                    else if (strcasecmp(schema_A[i].type, "DATETIME") == 0) where_type = 3;
                    break;
                }
            }
            // If not in A, check Table B
            if (!has_where_A) {
                for (int i = 0; i < cols_B; i++) {
                    if (strcasecmp(schema_B[i].name, w_col) == 0) {
                        has_where_B = 1; where_idx = i;
                        if (strcasecmp(schema_B[i].type, "INT") == 0) where_type = 1;
                        else if (strcasecmp(schema_B[i].type, "DECIMAL") == 0) where_type = 2;
                        else if (strcasecmp(schema_B[i].type, "DATETIME") == 0) where_type = 3;
                        break;
                    }
                }
            }
        }

        // =========================================================
        // RESOLVE THE SELECT LIST AGAINST BOTH SCHEMAS
        // A table-qualified entry (e.g. "students.id") is matched only to
        // the schema it names. An unqualified entry matches by name; if
        // that name exists on both sides, it's ambiguous and we error out
        // instead of guessing.
        // =========================================================
        int selected_A[100] = {0}, selected_B[100] = {0};
        int cols_to_send = 0;

        if (query->select_column_count == 0) {
            for (int i = 0; i < cols_A; i++) { selected_A[i] = 1; cols_to_send++; }
            for (int i = 0; i < cols_B; i++) { selected_B[i] = 1; cols_to_send++; }
        } else {
            for (int j = 0; j < query->select_column_count; j++) {
                const char* entry = query->select_columns[j];
                int is_qualified = strchr(entry, '.') != NULL;
                int match_A = -1, match_B = -1;
                for (int i = 0; i < cols_A; i++) if (join_col_matches_side(entry, query->table_name, schema_A[i].name)) { match_A = i; break; }
                for (int i = 0; i < cols_B; i++) if (join_col_matches_side(entry, query->join_table, schema_B[i].name)) { match_B = i; break; }

                if (!is_qualified && match_A != -1 && match_B != -1) {
                    char err[160]; snprintf(err, sizeof(err), "ERROR|Column '%s' is ambiguous between %s and %s; qualify it.\n", entry, query->table_name, query->join_table);
                    send(client_sock, err, strlen(err), 0);
                    unlock_join_locks(t_lock, t_lock_B); return;
                }
                if (match_A == -1 && match_B == -1) {
                    char err[128]; snprintf(err, sizeof(err), "ERROR|Column '%s' does not exist.\n", entry);
                    send(client_sock, err, strlen(err), 0);
                    unlock_join_locks(t_lock, t_lock_B); return;
                }
                if (match_A != -1 && !selected_A[match_A]) { selected_A[match_A] = 1; cols_to_send++; }
                if (match_B != -1 && !selected_B[match_B]) { selected_B[match_B] = 1; cols_to_send++; }
            }
        }

        // Open Table B's Pager
        char path_B[1024]; snprintf(path_B, sizeof(path_B), "%s/%s.dat", db_path, query->join_table);
        Pager* pager_B = pager_open(path_B);
        if (!pager_B) { 
            send(client_sock, "ERROR|Failed to open right table.\n", 34, 0); 
            unlock_join_locks(t_lock, t_lock_B); return;
        }

        char* net_ptr = net_buffer; int rows_sent = 0;

        // =========================================================
        // ROUTE 1A: Table B has the Primary Key! (Outer: A, Inner: B)
        // =========================================================
        if (pk_idx_B != -1 && join_idx_B == pk_idx_B) {
            // Table B's tree may have split in a session other than this one -
            // page 0 is only the root until the first split moves it.
            uint32_t root_id_B = load_root_page(db_path, query->join_table);
            for (uint32_t pid_A = 1; pid_A < pager->num_pages; pid_A++) {
                Page* page_A = get_page(pager, pid_A);
                if (page_A->header.page_type == 0) {
                    Slot* slots_A = (Slot*)page_A->data;
                    for (int s_A = 0; s_A < page_A->header.num_slots; s_A++) {
                        char* rec_A = &page_A->data[slots_A[s_A].offset - sizeof(PageHeader)];
                        TupleHeader head_A; memcpy(&head_A, rec_A, sizeof(TupleHeader));
                        if (head_A.expiration_timestamp < (uint64_t)time(NULL)) continue;

                        uint16_t off_A = sizeof(TupleHeader);
                        for(int i = 0; i < join_idx_A; i++) {
                            if (strcasecmp(schema_A[i].type, "INT") == 0) off_A += 4;
                            else if (strcasecmp(schema_A[i].type, "DECIMAL") == 0 || strcasecmp(schema_A[i].type, "DATETIME") == 0) off_A += 8;
                            else { uint16_t l; memcpy(&l, rec_A + off_A, 2); off_A += 2 + l; }
                        }
                        
                        IndexKey search_key; search_key.type = type_join;
                        if (type_join == 1) memcpy(&search_key.value.int_val, rec_A + off_A, 4);
                        else if (type_join == 2) memcpy(&search_key.value.dec_val, rec_A + off_A, 8);
                        else if (type_join == 3) memcpy(&search_key.value.dt_val, rec_A + off_A, 8);
                        else {
                            uint16_t str_len; memcpy(&str_len, rec_A + off_A, 2);
                            int cp_len = str_len < 31 ? str_len : 31; memcpy(search_key.value.str_val, rec_A + off_A + 2, cp_len);
                            search_key.value.str_val[cp_len] = '\0';
                        }

                        RecordID res_B;
                        if (btree_search(pager_B, root_id_B, search_key, &res_B)) {
                            Page* page_B = get_page(pager_B, res_B.page_num);
                            Slot* slots_B = (Slot*)page_B->data;
                            char* rec_B = &page_B->data[slots_B[res_B.slot_num].offset - sizeof(PageHeader)];
                            TupleHeader head_B; memcpy(&head_B, rec_B, sizeof(TupleHeader));
                            
                            if (head_B.expiration_timestamp >= (uint64_t)time(NULL)) {
                                
                                // --- APPLY WHERE FILTER ---
                                int passes_where = 1;
                                if (has_where_A) {
                                    uint16_t w_off = sizeof(TupleHeader);
                                    for (int i=0; i<where_idx; i++) {
                                        if (strcasecmp(schema_A[i].type, "INT") == 0) w_off += 4; else if (strcasecmp(schema_A[i].type, "DECIMAL") == 0 || strcasecmp(schema_A[i].type, "DATETIME") == 0) w_off += 8; else { uint16_t l; memcpy(&l, rec_A + w_off, 2); w_off += 2 + l; }
                                    }
                                    passes_where = evaluate_condition(where_type, rec_A + w_off, query->where_operator, query->where_value);
                                } else if (has_where_B) {
                                    uint16_t w_off = sizeof(TupleHeader);
                                    for (int i=0; i<where_idx; i++) {
                                        if (strcasecmp(schema_B[i].type, "INT") == 0) w_off += 4; else if (strcasecmp(schema_B[i].type, "DECIMAL") == 0 || strcasecmp(schema_B[i].type, "DATETIME") == 0) w_off += 8; else { uint16_t l; memcpy(&l, rec_B + w_off, 2); w_off += 2 + l; }
                                    }
                                    passes_where = evaluate_condition(where_type, rec_B + w_off, query->where_operator, query->where_value);
                                }

                                if (passes_where) {
                                    net_ptr += sprintf(net_ptr, "ROW|%d", cols_to_send);
                                    net_ptr = emit_join_columns(net_ptr, rec_A, schema_A, cols_A, selected_A);
                                    net_ptr = emit_join_columns(net_ptr, rec_B, schema_B, cols_B, selected_B);
                                    *net_ptr++ = '\n'; rows_sent++;
                                    if ((net_ptr - net_buffer) >= 60000) { send(client_sock, net_buffer, net_ptr - net_buffer, 0); net_ptr = net_buffer; }
                                }
                            }
                            unpin_page(pager_B, res_B.page_num, 0);
                        }
                    }
                }
                unpin_page(pager, pid_A, 0);
            }
        }
        // =========================================================
        // ROUTE 1B: Table A has the Primary Key! (Outer: B, Inner: A)
        // =========================================================
        else if (pk_idx_A != -1 && join_idx_A == pk_idx_A) {
            
            for (uint32_t pid_B = 1; pid_B < pager_B->num_pages; pid_B++) {
                Page* page_B = get_page(pager_B, pid_B);
                if (page_B->header.page_type == 0) {
                    Slot* slots_B = (Slot*)page_B->data;
                    for (int s_B = 0; s_B < page_B->header.num_slots; s_B++) {
                        char* rec_B = &page_B->data[slots_B[s_B].offset - sizeof(PageHeader)];
                        TupleHeader head_B; memcpy(&head_B, rec_B, sizeof(TupleHeader));
                        if (head_B.expiration_timestamp < (uint64_t)time(NULL)) continue;

                        uint16_t off_B = sizeof(TupleHeader);
                        for(int i = 0; i < join_idx_B; i++) {
                            if (strcasecmp(schema_B[i].type, "INT") == 0) off_B += 4;
                            else if (strcasecmp(schema_B[i].type, "DECIMAL") == 0 || strcasecmp(schema_B[i].type, "DATETIME") == 0) off_B += 8;
                            else { uint16_t l; memcpy(&l, rec_B + off_B, 2); off_B += 2 + l; }
                        }
                        
                        IndexKey search_key; search_key.type = type_join;
                        if (type_join == 1) memcpy(&search_key.value.int_val, rec_B + off_B, 4);
                        else if (type_join == 2) memcpy(&search_key.value.dec_val, rec_B + off_B, 8);
                        else if (type_join == 3) memcpy(&search_key.value.dt_val, rec_B + off_B, 8);
                        else {
                            uint16_t str_len; memcpy(&str_len, rec_B + off_B, 2);
                            int cp_len = str_len < 31 ? str_len : 31; memcpy(search_key.value.str_val, rec_B + off_B + 2, cp_len);
                            search_key.value.str_val[cp_len] = '\0';
                        }

                        RecordID res_A;
                        if (btree_search(pager, root_page_id, search_key, &res_A)) {
                            Page* page_A = get_page(pager, res_A.page_num);
                            Slot* slots_A = (Slot*)page_A->data;
                            char* rec_A = &page_A->data[slots_A[res_A.slot_num].offset - sizeof(PageHeader)];
                            TupleHeader head_A; memcpy(&head_A, rec_A, sizeof(TupleHeader));
                            
                            if (head_A.expiration_timestamp >= (uint64_t)time(NULL)) {
                                
                                // --- APPLY WHERE FILTER ---
                                int passes_where = 1;
                                if (has_where_A) {
                                    uint16_t w_off = sizeof(TupleHeader);
                                    for (int i=0; i<where_idx; i++) {
                                        if (strcasecmp(schema_A[i].type, "INT") == 0) w_off += 4; else if (strcasecmp(schema_A[i].type, "DECIMAL") == 0 || strcasecmp(schema_A[i].type, "DATETIME") == 0) w_off += 8; else { uint16_t l; memcpy(&l, rec_A + w_off, 2); w_off += 2 + l; }
                                    }
                                    passes_where = evaluate_condition(where_type, rec_A + w_off, query->where_operator, query->where_value);
                                } else if (has_where_B) {
                                    uint16_t w_off = sizeof(TupleHeader);
                                    for (int i=0; i<where_idx; i++) {
                                        if (strcasecmp(schema_B[i].type, "INT") == 0) w_off += 4; else if (strcasecmp(schema_B[i].type, "DECIMAL") == 0 || strcasecmp(schema_B[i].type, "DATETIME") == 0) w_off += 8; else { uint16_t l; memcpy(&l, rec_B + w_off, 2); w_off += 2 + l; }
                                    }
                                    passes_where = evaluate_condition(where_type, rec_B + w_off, query->where_operator, query->where_value);
                                }

                                if (passes_where) {
                                    net_ptr += sprintf(net_ptr, "ROW|%d", cols_to_send);
                                    net_ptr = emit_join_columns(net_ptr, rec_A, schema_A, cols_A, selected_A);
                                    net_ptr = emit_join_columns(net_ptr, rec_B, schema_B, cols_B, selected_B);
                                    *net_ptr++ = '\n'; rows_sent++;
                                    if ((net_ptr - net_buffer) >= 60000) { send(client_sock, net_buffer, net_ptr - net_buffer, 0); net_ptr = net_buffer; }
                                }
                            }
                            unpin_page(pager, res_A.page_num, 0);
                        }
                    }
                }
                unpin_page(pager_B, pid_B, 0);
            }
        }
        // =========================================================
        // ROUTE 2: BLOCK NESTED LOOP JOIN (Fallback for non-indexed tables)
        // =========================================================
        else {
            for (uint32_t pid_A = 1; pid_A < pager->num_pages; pid_A++) {
                Page* page_A = get_page(pager, pid_A);
                if (page_A->header.page_type == 0) {
                    Slot* slots_A = (Slot*)page_A->data;
                    for (int s_A = 0; s_A < page_A->header.num_slots; s_A++) {
                        char* rec_A = &page_A->data[slots_A[s_A].offset - sizeof(PageHeader)];
                        TupleHeader head_A; memcpy(&head_A, rec_A, sizeof(TupleHeader));
                        if (head_A.expiration_timestamp < (uint64_t)time(NULL)) continue;

                        uint16_t off_A = sizeof(TupleHeader);
                        for(int i = 0; i < join_idx_A; i++) {
                            if (strcasecmp(schema_A[i].type, "INT") == 0) off_A += 4;
                            else if (strcasecmp(schema_A[i].type, "DECIMAL") == 0 || strcasecmp(schema_A[i].type, "DATETIME") == 0) off_A += 8;
                            else { uint16_t l; memcpy(&l, rec_A + off_A, 2); off_A += 2 + l; }
                        }
                        char* val_A_ptr = rec_A + off_A;

                        // Scan Table B
                        for (uint32_t pid_B = 1; pid_B < pager_B->num_pages; pid_B++) {
                            Page* page_B = get_page(pager_B, pid_B);
                            if (page_B->header.page_type == 0) {
                                Slot* slots_B = (Slot*)page_B->data;
                                for (int s_B = 0; s_B < page_B->header.num_slots; s_B++) {
                                    char* rec_B = &page_B->data[slots_B[s_B].offset - sizeof(PageHeader)];
                                    TupleHeader head_B; memcpy(&head_B, rec_B, sizeof(TupleHeader));
                                    if (head_B.expiration_timestamp < (uint64_t)time(NULL)) continue;

                                    uint16_t off_B = sizeof(TupleHeader);
                                    for(int i = 0; i < join_idx_B; i++) {
                                        if (strcasecmp(schema_B[i].type, "INT") == 0) off_B += 4;
                                        else if (strcasecmp(schema_B[i].type, "DECIMAL") == 0 || strcasecmp(schema_B[i].type, "DATETIME") == 0) off_B += 8;
                                        else { uint16_t l; memcpy(&l, rec_B + off_B, 2); off_B += 2 + l; }
                                    }
                                    char* val_B_ptr = rec_B + off_B;

                                    // COMPARE
                                    int match = 0;
                                    if (type_join == 1) {
                                        int32_t a, b; memcpy(&a, val_A_ptr, 4); memcpy(&b, val_B_ptr, 4); match = (a == b);
                                    } else if (type_join == 2 || type_join == 3) {
                                        int64_t a, b; memcpy(&a, val_A_ptr, 8); memcpy(&b, val_B_ptr, 8); match = (a == b);
                                    } else {
                                        uint16_t lenA, lenB; memcpy(&lenA, val_A_ptr, 2); memcpy(&lenB, val_B_ptr, 2);
                                        if (lenA == lenB && memcmp(val_A_ptr + 2, val_B_ptr + 2, lenA) == 0) match = 1;
                                    }

                                    if (match) {
                                        // --- APPLY WHERE FILTER ---
                                        int passes_where = 1;
                                        if (has_where_A) {
                                            uint16_t w_off = sizeof(TupleHeader);
                                            for (int i=0; i<where_idx; i++) {
                                                if (strcasecmp(schema_A[i].type, "INT") == 0) w_off += 4; else if (strcasecmp(schema_A[i].type, "DECIMAL") == 0 || strcasecmp(schema_A[i].type, "DATETIME") == 0) w_off += 8; else { uint16_t l; memcpy(&l, rec_A + w_off, 2); w_off += 2 + l; }
                                            }
                                            passes_where = evaluate_condition(where_type, rec_A + w_off, query->where_operator, query->where_value);
                                        } else if (has_where_B) {
                                            uint16_t w_off = sizeof(TupleHeader);
                                            for (int i=0; i<where_idx; i++) {
                                                if (strcasecmp(schema_B[i].type, "INT") == 0) w_off += 4; else if (strcasecmp(schema_B[i].type, "DECIMAL") == 0 || strcasecmp(schema_B[i].type, "DATETIME") == 0) w_off += 8; else { uint16_t l; memcpy(&l, rec_B + w_off, 2); w_off += 2 + l; }
                                            }
                                            passes_where = evaluate_condition(where_type, rec_B + w_off, query->where_operator, query->where_value);
                                        }

                                        if (passes_where) {
                                            net_ptr += sprintf(net_ptr, "ROW|%d", cols_to_send);
                                            net_ptr = emit_join_columns(net_ptr, rec_A, schema_A, cols_A, selected_A);
                                            net_ptr = emit_join_columns(net_ptr, rec_B, schema_B, cols_B, selected_B);
                                            *net_ptr++ = '\n'; rows_sent++;
                                            if ((net_ptr - net_buffer) >= 60000) { send(client_sock, net_buffer, net_ptr - net_buffer, 0); net_ptr = net_buffer; }
                                        }
                                    }
                                }
                            }
                            unpin_page(pager_B, pid_B, 0);
                        }
                    }
                }
                unpin_page(pager, pid_A, 0);
            }
        }

        if (net_ptr > net_buffer) send(client_sock, net_buffer, net_ptr - net_buffer, 0);
        
        char done_msg[128]; snprintf(done_msg, sizeof(done_msg), "DONE|%d rows returned from Join.\n", rows_sent);
        send(client_sock, done_msg, strlen(done_msg), 0);
        
        // Not pager_close(pager_B) - it's the shared pager for table B, not
        // this join's to destroy.

        unlock_join_locks(t_lock, t_lock_B); // Release both (or the one, for a self-join)
        return;
    }






    char db_path[512];
    snprintf(db_path, sizeof(db_path), "%s/%s", DATA_DIR, current_db);

    static __thread char cached_table[256] = "";
    static __thread ColumnDef cached_schema[100];
    static __thread int cached_num_cols = 0;
    static __thread int cached_types[100]; 

    if (strcmp(cached_table, query->table_name) != 0) {
        cached_num_cols = load_schema(db_path, query->table_name, cached_schema);
        if (cached_num_cols != -1) {
            strcpy(cached_table, query->table_name);
            for (int i = 0; i < cached_num_cols; i++) {
                trim_string(cached_schema[i].name); trim_string(cached_schema[i].type);
                if (strcasecmp(cached_schema[i].type, "INT") == 0) cached_types[i] = 1;
                else if (strcasecmp(cached_schema[i].type, "DECIMAL") == 0) cached_types[i] = 2;
                else if (strcasecmp(cached_schema[i].type, "DATETIME") == 0) cached_types[i] = 3;
                else cached_types[i] = 4; 
            }
        } else cached_table[0] = '\0';
    }

    if (cached_num_cols == -1) {
        send(client_sock, "ERROR|Table does not exist.\n", 28, 0); 
        pthread_rwlock_unlock(t_lock);
        return;
    }

    if (query->select_column_count > 0) {
        for (int j = 0; j < query->select_column_count; j++) {
            int found = 0; const char* col_name = get_base_col(query->select_columns[j]);
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(col_name, cached_schema[i].name) == 0) { found = 1; break; }
            }
            if (!found) {
                char err[128]; snprintf(err, 128, "ERROR|Column '%s' does not exist.\n", col_name);
                send(client_sock, err, strlen(err), 0); 
                pthread_rwlock_unlock(t_lock);
                return;
            }
        }
    }

    
    // =========================================================
    // DYNAMIC PK LOOKUP & HEAP TABLE DETECTION
    // =========================================================
    int pk_idx = 0; 
    int has_pk = 0; 
    for (int i = 0; i < cached_num_cols; i++) {
        if (cached_schema[i].is_primary_key) { pk_idx = i; has_pk = 1; break; }
    }

    int cols_to_send = query->select_column_count == 0 ? cached_num_cols : query->select_column_count;
    
    int use_btree = 0;
    const char* w_col = query->has_where ? get_base_col(query->where_column) : "";
    
    // Trigger B-Tree ONLY if the table actually HAS a primary key, AND they search on it!
    if (has_pk && query->has_where && strcasecmp(w_col, cached_schema[pk_idx].name) == 0 && strcmp(query->where_operator, "=") == 0) {
        use_btree = 1; 
    }


    if (use_btree) {
        IndexKey search_key;
        if (cached_types[pk_idx] == 1) { search_key.type = 1; search_key.value.int_val = atoi(query->where_value); } 
        else if (cached_types[pk_idx] == 2) { search_key.type = 2; search_key.value.dec_val = atof(query->where_value); }
        else if (cached_types[pk_idx] == 3) { 
            search_key.type = 3; 
            struct tm tm_info; memset(&tm_info, 0, sizeof(struct tm));
            if (strptime(query->where_value, "%Y-%m-%d %H:%M:%S", &tm_info) != NULL ||
                strptime(query->where_value, "%Y-%m-%d", &tm_info) != NULL) {
                search_key.value.dt_val = (int64_t)mktime(&tm_info);
            } else {
                search_key.value.dt_val = atoll(query->where_value);
            }
        }
        else { search_key.type = 4; strncpy(search_key.value.str_val, query->where_value, 32); }

        RecordID result;
        if (btree_search(pager, root_page_id, search_key, &result)) {
            Page* data_page = get_page(pager, result.page_num);
            Slot* slots = (Slot*)data_page->data;
            char* raw_record = &data_page->data[slots[result.slot_num].offset - sizeof(PageHeader)];
            
            TupleHeader header; memcpy(&header, raw_record, sizeof(TupleHeader));
            if (header.expiration_timestamp >= (uint64_t)time(NULL)) {
                char* ptr = net_buffer;
                ptr += sprintf(ptr, "ROW|%d", cols_to_send);
                uint16_t offset = sizeof(TupleHeader); 

                for (int i = 0; i < cached_num_cols; i++) {
                    int selected = (query->select_column_count == 0);
                    if (!selected) {
                        for(int j=0; j<query->select_column_count; j++){
                            if(strcasecmp(get_base_col(query->select_columns[j]), cached_schema[i].name) == 0) { selected = 1; break; }
                        }
                    }

                    if (selected) {
                        *ptr++ = '|';
                        int name_len = strlen(cached_schema[i].name);
                        memcpy(ptr, cached_schema[i].name, name_len); ptr += name_len;
                        *ptr++ = '|';

                        if (cached_types[i] == 1) {
                            int32_t val; memcpy(&val, raw_record + offset, 4);
                            ptr += sprintf(ptr, "%d", val); offset += 4;
                        } else if (cached_types[i] == 2) {
                            double val; memcpy(&val, raw_record + offset, 8);
                            ptr += sprintf(ptr, "%g", val); offset += 8; 
                        } else if (cached_types[i] == 3) {
                            // BEAUTIFUL DATETIME FORMATTING!
                            int64_t val; memcpy(&val, raw_record + offset, 8);
                            time_t t = (time_t)val;
                            struct tm *tm_info = localtime(&t);
                            char time_str[32];
                            strftime(time_str, 32, "%Y-%m-%d %H:%M:%S", tm_info);
                            ptr += sprintf(ptr, "%s", time_str); offset += 8;
                        } else {
                            uint16_t len; memcpy(&len, raw_record + offset, 2); offset += 2;
                            memcpy(ptr, raw_record + offset, len); ptr += len; offset += len;
                        }
                    } else {
                        if (cached_types[i] == 1) offset += 4;
                        else if (cached_types[i] == 2) offset += 8;
                        else if (cached_types[i] == 3) offset += 8;
                        else { uint16_t len; memcpy(&len, raw_record + offset, 2); offset += 2 + len; }
                    }
                }
                *ptr++ = '\n';
                send(client_sock, net_buffer, ptr - net_buffer, 0);
                send(client_sock, "DONE|Query executed successfully.\n", 34, 0);
            } else {
                 send(client_sock, "DONE|0 rows returned.\n", 22, 0);
            }
            unpin_page(pager, result.page_num, 0); 
        } else {
            send(client_sock, "DONE|0 rows returned.\n", 22, 0);
        }

    } else {
        // ----------------------------------------------------
        // PATH B: RANGE / SORT / NON-PK (Full Table Scan)
        // ----------------------------------------------------
        int where_idx = -1;
        if (query->has_where) {
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(cached_schema[i].name, w_col) == 0) { where_idx = i; break; }
            }
        }

        int order_idx = -1;
        if (query->has_order_by) {
            const char* o_col = get_base_col(query->order_by_column);
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(cached_schema[i].name, o_col) == 0) { order_idx = i; break; }
            }
        }

        SortRow* results = malloc(sizeof(SortRow) * 10000); 
        int match_count = 0;

        for (uint32_t pid = 1; pid < pager->num_pages; pid++) {
            Page* page = get_page(pager, pid);
            
            if (page->header.page_type == 0) { // Data Page
                Slot* slots = (Slot*)page->data;
                for (int s = 0; s < page->header.num_slots; s++) {
                    char* raw_record = &page->data[slots[s].offset - sizeof(PageHeader)];
                    
                    TupleHeader header; memcpy(&header, raw_record, sizeof(TupleHeader));
                    if (header.expiration_timestamp < (uint64_t)time(NULL)) continue;

                    uint16_t offsets[100];
                    uint16_t cur_off = sizeof(TupleHeader);
                    for(int i = 0; i < cached_num_cols; i++) {
                        offsets[i] = cur_off;
                        if(cached_types[i] == 1) cur_off += 4;
                        else if(cached_types[i] == 2) cur_off += 8;
                        else if(cached_types[i] == 3) cur_off += 8;
                        else { uint16_t len; memcpy(&len, raw_record + cur_off, 2); cur_off += 2 + len; }
                    }

                    int match = 1;
                    if (query->has_where && where_idx != -1) {
                        match = evaluate_condition(cached_types[where_idx], raw_record + offsets[where_idx], query->where_operator, query->where_value);
                    } else if (query->has_where && where_idx == -1) {
                        match = 0; // If column not found, it shouldn't match anything!
                    }

                    if (match && match_count < 10000) {
                        char temp_row[2048]; char* ptr = temp_row;
                        ptr += sprintf(ptr, "ROW|%d", cols_to_send);

                        for (int i = 0; i < cached_num_cols; i++) {
                            int selected = (query->select_column_count == 0);
                            if (!selected) {
                                for(int j=0; j<query->select_column_count; j++){
                                    if(strcasecmp(get_base_col(query->select_columns[j]), cached_schema[i].name) == 0) { selected = 1; break; }
                                }
                            }
                            if (selected) {
                                *ptr++ = '|';
                                int n_len = strlen(cached_schema[i].name);
                                memcpy(ptr, cached_schema[i].name, n_len); ptr += n_len;
                                *ptr++ = '|';

                                if (cached_types[i] == 1) {
                                    int32_t v; memcpy(&v, raw_record + offsets[i], 4);
                                    ptr += sprintf(ptr, "%d", v);
                                } else if (cached_types[i] == 2) {
                                    double v; memcpy(&v, raw_record + offsets[i], 8);
                                    ptr += sprintf(ptr, "%g", v); 
                                } else if (cached_types[i] == 3) {
                                    int64_t v; memcpy(&v, raw_record + offsets[i], 8);
                                    time_t t = (time_t)v;
                                    struct tm *tm_info = localtime(&t);
                                    char time_str[32];
                                    strftime(time_str, 32, "%Y-%m-%d %H:%M:%S", tm_info);
                                    ptr += sprintf(ptr, "%s", time_str);
                                } else {
                                    uint16_t len; memcpy(&len, raw_record + offsets[i], 2);
                                    memcpy(ptr, raw_record + offsets[i] + 2, len); ptr += len;
                                }
                            }
                        }
                        *ptr = '\0'; 
                        results[match_count].row_str = strdup(temp_row);

                        if (order_idx != -1) {
                            if (cached_types[order_idx] == 1) {
                                int32_t v; memcpy(&v, raw_record + offsets[order_idx], 4);
                                results[match_count].sort_num = (double)v; g_sort_type = 1;
                            } else if (cached_types[order_idx] == 2) {
                                double v; memcpy(&v, raw_record + offsets[order_idx], 8);
                                results[match_count].sort_num = v; g_sort_type = 1;
                            } else if (cached_types[order_idx] == 3) {
                                int64_t v; memcpy(&v, raw_record + offsets[order_idx], 8);
                                results[match_count].sort_num = (double)v; g_sort_type = 1;
                            } else {
                                uint16_t len; memcpy(&len, raw_record + offsets[order_idx], 2);
                                int cp_len = len < 63 ? len : 63;
                                memcpy(results[match_count].sort_str, raw_record + offsets[order_idx] + 2, cp_len);
                                results[match_count].sort_str[cp_len] = '\0'; g_sort_type = 2;
                            }
                        }
                        match_count++;
                    }
                }
            }
            unpin_page(pager, pid, 0);
        }

        if (query->has_order_by && match_count > 0) {
            qsort(results, match_count, sizeof(SortRow), compare_sort_rows);
        }

        char* net_ptr = net_buffer;
        for (int i = 0; i < match_count; i++) {
            int rlen = strlen(results[i].row_str);
            if ((net_ptr - net_buffer) + rlen + 2 >= sizeof(net_buffer)) {
                send(client_sock, net_buffer, net_ptr - net_buffer, 0);
                net_ptr = net_buffer;
            }
            memcpy(net_ptr, results[i].row_str, rlen); net_ptr += rlen;
            *net_ptr++ = '\n'; 
            free(results[i].row_str);
        }
        
        const char* done_msg = "DONE|Query executed successfully.\n";
        memcpy(net_ptr, done_msg, strlen(done_msg)); net_ptr += strlen(done_msg);
        send(client_sock, net_buffer, net_ptr - net_buffer, 0);

        free(results);
    }

    pthread_rwlock_unlock(t_lock);
}