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
    char cmd[512]; char filepath[512];
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
    
    snprintf(filepath, sizeof(filepath), "%s/%s/%s.schema", DATA_DIR, current_db, query->table_name);
    if (remove(filepath) != 0) return -1; // If schema isn't there, table doesn't exist!
    
    snprintf(filepath, sizeof(filepath), "%s/%s/%s.dat", DATA_DIR, current_db, query->table_name);
    remove(filepath);
    return 0;
}

void execute_show_db(int client_sock) {
    ensure_sandbox();
    DIR *d; struct dirent *dir;
    char buffer[4096] = "ROW|1|Database\n";
    d = opendir(DATA_DIR);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (dir->d_type == DT_DIR && strcmp(dir->d_name, ".") != 0 && strcmp(dir->d_name, "..") != 0) {
                char row[256];
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
    char buffer[4096] = "ROW|1|Table\n";
    d = opendir(filepath);
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            if (strstr(dir->d_name, ".schema")) {
                char row[256]; char t_name[256];
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

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", DATA_DIR, current_db);
    mkdir(path, 0777); 
    
    if (save_schema(path, query->table_name, query->columns, query->column_count) == 0) {
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s.dat", path, query->table_name);
        Pager* pager = pager_open(filepath);
        Page* root_page = get_page(pager, 0);
        root_page->header.page_type = 1;
        BTreeNode* root_node = (BTreeNode*)root_page->data;
        root_node->is_leaf = 1; root_node->is_root = 1; root_node->num_keys = 0;
        unpin_page(pager, 0, 1);
        pager_close(pager);
        return 0; // SUCCESS
    }
    return -1; // FAILURE
}

int execute_delete(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) return -1;
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s/%s.dat", DATA_DIR, current_db, query->table_name);
    
    struct stat st = {0};
    if (stat(filepath, &st) == -1) return -1; // File missing!

    remove(filepath);
    Pager* pager = pager_open(filepath);
    Page* root_page = get_page(pager, 0);
    root_page->header.page_type = 1;
    BTreeNode* root_node = (BTreeNode*)root_page->data;
    root_node->is_leaf = 1; root_node->is_root = 1; root_node->num_keys = 0;
    unpin_page(pager, 0, 1);
    pager_close(pager);
    return 0;
}


void execute_insert(const char* current_db, Pager* pager, uint32_t* root_page_id, uint32_t* active_data_page, ParsedQuery* query, int client_sock) {
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
        return -1;
    }
    
    if (query->bulk_insert_ptr == NULL) return;

    char* ptr = query->bulk_insert_ptr;
    char tuple_buffer[MAX_TUPLE_SIZE];

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
                free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL; return -1;
            }

            // SEMANTIC CONSTRAINT: NOT NULL Check
            for (int i = 0; i < cached_num_cols; i++) {
                if (cached_schema[i].is_not_null || cached_schema[i].is_primary_key) {
                    if (strlen(query->values[i]) == 0 || strcasecmp(query->values[i], "NULL") == 0) {
                        char err[128]; snprintf(err, 128, "ERROR|NOT NULL constraint violation on column '%s'.\n", cached_schema[i].name);
                        send(client_sock, err, strlen(err), 0);
                        free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL; return -1;
                    }
                }
            }
            // --------------------------

            // Extract Primary Key for validation
            IndexKey pk;
            if (cached_types[0] == 1) {
                if (!safe_parse_int(query->values[0], &pk.value.int_val)) goto type_err;
                pk.type = 1;
            } else if (cached_types[0] == 2) {
                if (!safe_parse_double(query->values[0], &pk.value.dec_val)) goto type_err;
                pk.type = 2;
            } else {
                pk.type = 4; strncpy(pk.value.str_val, query->values[0], 32);
            }

            // SEMANTIC CONSTRAINT: Duplicate Key Check!
            RecordID dummy;
            if (btree_search(pager, *root_page_id, pk, &dummy)) {
                const char* err = "ERROR|Duplicate Primary Key constraint violation.\n";
                send(client_sock, err, strlen(err), 0);
                free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL; return -1;
            }

            // SEMANTIC CONSTRAINT: Type Safety Check!
            int tuple_size = serialize_row(query, cached_types, tuple_buffer);
            if (tuple_size == -1) {
type_err:
                const char* err = "ERROR|Invalid datatype or missing required field.\n";
                send(client_sock, err, strlen(err), 0);
                free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL; return -1;
            }

            RecordID record_location = append_to_data_page(pager, active_data_page, tuple_buffer, tuple_size);
            btree_insert(pager, root_page_id, pk, record_location);
        }
        ptr++;
    }

    free(query->bulk_insert_ptr); query->bulk_insert_ptr = NULL;
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
    } else if (type == 3) { // DATETIME
        int64_t val; memcpy(&val, record_ptr, 8);
        int64_t cmp = atoll(val_str); 
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

void execute_select(const char* current_db, Pager* pager, uint32_t root_page_id, ParsedQuery* query, int client_sock) {
    char net_buffer[65536];  net_buffer[0] = '\0';
    if (strlen(current_db) == 0) {
        send(client_sock, "ERROR|No database selected.\n", 28, 0); return;
    }
    if (query->has_join) {
        send(client_sock, "DONE|0 rows returned.\n", 22, 0); return;
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
        send(client_sock, "ERROR|Table does not exist.\n", 28, 0); return;
    }

    if (query->select_column_count > 0) {
        for (int j = 0; j < query->select_column_count; j++) {
            int found = 0; const char* col_name = get_base_col(query->select_columns[j]);
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(col_name, cached_schema[i].name) == 0) { found = 1; break; }
            }
            if (!found) {
                char err[128]; snprintf(err, 128, "ERROR|Column '%s' does not exist.\n", col_name);
                send(client_sock, err, strlen(err), 0); return;
            }
        }
    }

    int cols_to_send = query->select_column_count == 0 ? cached_num_cols : query->select_column_count;
    int use_btree = 0;
    const char* w_col = query->has_where ? get_base_col(query->where_column) : "";
    if (query->has_where && strcasecmp(w_col, cached_schema[0].name) == 0 && strcmp(query->where_operator, "=") == 0) use_btree = 1; 

    if (use_btree) {
        IndexKey search_key;
        if (cached_types[0] == 1) { search_key.type = 1; search_key.value.int_val = atoi(query->where_value); } 
        else if (cached_types[0] == 2) { search_key.type = 2; search_key.value.dec_val = atof(query->where_value); }
        else if (cached_types[0] == 3) { search_key.type = 3; search_key.value.dt_val = atoll(query->where_value); }
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
}