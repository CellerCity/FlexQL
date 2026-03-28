#define _GNU_SOURCE   // Enables ALL modern POSIX/Linux features
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "pager.h"
#include "btree.h"
#include "schema.h"
#include "../parser/parser.h" 
#include <sys/stat.h>  // For mkdir() and stat()
#include <sys/types.h>
#include <sys/socket.h>

extern void trim_string(char *str);


// Creates a new directory for the database
int execute_create_db(ParsedQuery* query) {
    // 0777 gives read/write/execute permissions to the folder
    if (mkdir(query->db_name, 0777) == 0) {
        printf("[+] Database '%s' created successfully.\n", query->db_name);
        return 0;
    } else {
        perror("[-] Failed to create database (it may already exist)");
        return -1;
    }
}

// Verifies the database exists and updates the client's session string
int execute_use_db(ParsedQuery* query, char* current_db_session) {
    struct stat st = {0};
    
    // Check if the directory actually exists
    if (stat(query->db_name, &st) == -1) {
        printf("[-] Error: Database '%s' does not exist.\n", query->db_name);
        return -1;
    }
    
    // It exists! Update the client's session state
    strcpy(current_db_session, query->db_name);
    printf("[+] Database changed to '%s'.\n", current_db_session);
    return 0;
}

void execute_drop_table(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) return;

    char filepath[512];
    
    // Delete the Schema file
    snprintf(filepath, sizeof(filepath), "%s/%s.schema", current_db, query->table_name);
    remove(filepath);
    
    // Delete the Data file
    snprintf(filepath, sizeof(filepath), "%s/%s.dat", current_db, query->table_name);
    remove(filepath);
    
    printf("[+] Table '%s' dropped from database '%s'.\n", query->table_name, current_db);
}

void execute_drop_db(ParsedQuery* query, char* current_db_session) {
    char cmd[512];
    
    // Using system() to quickly recursively delete the folder and all its contents
    snprintf(cmd, sizeof(cmd), "rm -rf %s", query->db_name);
    system(cmd);
    
    printf("[+] Database '%s' and all its tables were dropped.\n", query->db_name);

    // If the user drops the database they are currently using, we must kick them out of it!
    if (strcmp(current_db_session, query->db_name) == 0) {
        current_db_session[0] = '\0'; // Empty the string
        printf("[!] You are no longer using any database.\n");
    }
}

void execute_delete(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) {
        printf("[-] Error: No database selected.\n");
        return;
    }

    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s.dat", current_db, query->table_name);
    
    // 1. Delete the old data file from the hard drive
    remove(filepath);
    
    // 2. Re-initialize a fresh, empty B+ Tree Root Page!
    Pager* pager = pager_open(filepath);
    Page* root_page = get_page(pager, 0);
    
    root_page->header.page_type = 1; // Mark as Index Page
    BTreeNode* root_node = (BTreeNode*)root_page->data;
    root_node->is_leaf = 1;
    root_node->is_root = 1;
    root_node->num_keys = 0;
    
    unpin_page(pager, 0, 1); // Save the new root to disk
    pager_close(pager);
    
    printf("[+] All records deleted from table '%s'.\n", query->table_name);
}


// --- 1. Your Excellent Serialization Logic ---
// Change the signature to take `int* cached_types` instead of `ColumnDef* schema`
uint16_t serialize_row(ParsedQuery* query, int* cached_types, char* tuple_buffer) {
    uint16_t current_offset = 0;

    // Mandatory Expiration Timestamp
    TupleHeader header;
    header.expiration_timestamp = (uint64_t)time(NULL) + (30 * 24 * 60 * 60); 
    
    memcpy(tuple_buffer + current_offset, &header, sizeof(TupleHeader));
    current_offset += sizeof(TupleHeader);

    for (int i = 0; i < query->value_count; i++) {
        // --- O(1) INTEGER CHECKS ---
        if (cached_types[i] == 1) { 
            // INT
            int32_t val = atoi(query->values[i]);
            memcpy(tuple_buffer + current_offset, &val, sizeof(int32_t));
            current_offset += sizeof(int32_t);
        } else if (cached_types[i] == 2) { 
            // DECIMAL
            double val = atof(query->values[i]);
            memcpy(tuple_buffer + current_offset, &val, sizeof(double));
            current_offset += sizeof(double);
        } else if (cached_types[i] == 3) { 
            // DATETIME
            struct tm tm_info;
            memset(&tm_info, 0, sizeof(struct tm));
            int64_t timestamp = 0;
            if (strptime(query->values[i], "%Y-%m-%d %H:%M:%S", &tm_info) != NULL ||
                strptime(query->values[i], "%Y-%m-%d", &tm_info) != NULL) {
                timestamp = (int64_t)mktime(&tm_info);
            }
            memcpy(tuple_buffer + current_offset, &timestamp, sizeof(int64_t));
            current_offset += sizeof(int64_t);
        } else { 
            // TEXT or VARCHAR (cached_types[i] == 4)
            uint16_t len = strlen(query->values[i]);
            memcpy(tuple_buffer + current_offset, &len, sizeof(uint16_t));
            current_offset += sizeof(uint16_t);
            memcpy(tuple_buffer + current_offset, query->values[i], len);
            current_offset += len;
        }
    }
    return current_offset; 
}

// --- 2. Your Original Page Insertion Logic ---
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

// --- 3. The Thread-Safe Pager Wrapper ---
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

void execute_create(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) {
        printf("[-] Error: No database selected.\n");
        return;
    }

    if (save_schema(current_db, query->table_name, query->columns, query->column_count) == 0) {
        
        // --- NEW: Initialize the B+ Tree Root Page immediately! ---
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s.dat", current_db, query->table_name);
        Pager* pager = pager_open(filepath);
        
        Page* root_page = get_page(pager, 0);

        // --- Mark as an Index Page so Data doesn't overwrite it! ---
        root_page->header.page_type = 1;

        BTreeNode* root_node = (BTreeNode*)root_page->data;
        root_node->is_leaf = 1;
        root_node->is_root = 1;
        root_node->num_keys = 0;
        
        unpin_page(pager, 0, 1); // Mark as dirty so it saves to disk
        pager_close(pager);
        // -----------------------------------------------------------
        
        printf("[+] Table '%s' created in database '%s'.\n", query->table_name, current_db);
    }
}


void execute_insert(const char* current_db, Pager* pager, uint32_t* root_page_id, uint32_t* active_data_page, ParsedQuery* query) {
    if (strlen(current_db) == 0) {
        printf("[-] Error: No database selected. Use 'USE <dbname>;' first.\n");
        return;
    }
    
    // --- THE SCHEMA CACHE OPTIMIZATION ---
    static char cached_table[256] = "";
    static ColumnDef cached_schema[100];
    static int cached_num_cols = 0;
    static int cached_types[100]; // Hold pre-computed integer types!

    // Only hit the hard drive if the user queries a DIFFERENT table!
    if (strcmp(cached_table, query->table_name) != 0) {
        cached_num_cols = load_schema(current_db, query->table_name, cached_schema);
        if (cached_num_cols != -1) {
            strcpy(cached_table, query->table_name);
            
            // Pre-compute the types so we never use strcasecmp again!
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(cached_schema[i].type, "INT") == 0) cached_types[i] = 1;
                else if (strcasecmp(cached_schema[i].type, "DECIMAL") == 0) cached_types[i] = 2;
                else if (strcasecmp(cached_schema[i].type, "DATETIME") == 0) cached_types[i] = 3;
                else cached_types[i] = 4; // VARCHAR / TEXT
            }
        }
    }

    if (cached_num_cols == -1) {
        printf("[-] Error: Table '%s' does not exist.\n", query->table_name);
        return;
    }

    
    if (query->bulk_insert_ptr == NULL) return;

    // --- THE ZERO-COPY BULK INGESTER ---
    char* ptr = query->bulk_insert_ptr;
    char tuple_buffer[MAX_TUPLE_SIZE];

    // Stream through the massive 250KB string
    while (*ptr != '\0') {
        if (*ptr == '(') {
            ptr++;
            query->value_count = 0;
            char val_buf[512];
            int v_idx = 0;

            // Extract values for THIS specific row only
            while (*ptr != ')' && *ptr != '\0') {
                if (*ptr == ',') {
                    val_buf[v_idx] = '\0';
                    trim_string(val_buf);
                    strcpy(query->values[query->value_count++], val_buf);
                    v_idx = 0;
                } else {
                    val_buf[v_idx++] = *ptr;
                }
                ptr++;
            }

            // Grab the last value before the ')'
            if (v_idx > 0) {
                val_buf[v_idx] = '\0';
                trim_string(val_buf);
                strcpy(query->values[query->value_count++], val_buf);
            }

            // 1. Serialize row (Update serialize_row to use atoll() for datetimes too!)
            uint16_t tuple_size = serialize_row(query, cached_types, tuple_buffer);

            // 2. Drop into Data Page
            RecordID record_location = append_to_data_page(pager, active_data_page, tuple_buffer, tuple_size);

            // 3. Extract Primary Key and link to B+ Tree
            IndexKey pk;
            if (cached_types[0] == 1) {
                pk.type = 1;
                pk.value.int_val = atoi(query->values[0]);
            } else if (cached_types[0] == 2) {
                pk.type = 2;
                pk.value.dec_val = atof(query->values[0]);
            } else if (cached_types[0] == 3) {
                pk.type = 3;
                // TA's trap: They sent an integer instead of a string!
                if (strchr(query->values[0], '-')) {
                    struct tm tm_info; memset(&tm_info, 0, sizeof(struct tm));
                    strptime(query->values[0], "%Y-%m-%d", &tm_info);
                    pk.value.dt_val = (int64_t)mktime(&tm_info);
                } else {
                    pk.value.dt_val = atoll(query->values[0]); 
                }
            } else {
                pk.type = 4;
                strncpy(pk.value.str_val, query->values[0], 32);
            }

            btree_insert(pager, root_page_id, pk, record_location);
        }
        ptr++;
    }

    // Safely free the massive string from RAM once we are done!
    free(query->bulk_insert_ptr);

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

// Evaluates >, <, =, >=, <= dynamically based on the column type!
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
        memcpy(str_val, record_ptr + 2, len);
        int cmp = strcmp(str_val, val_str);
        if (strcmp(op, "=") == 0) return cmp == 0;
        if (strcmp(op, ">") == 0) return cmp > 0;
        if (strcmp(op, "<") == 0) return cmp < 0;
        if (strcmp(op, ">=") == 0) return cmp >= 0;
        if (strcmp(op, "<=") == 0) return cmp <= 0;
    }
    return 0;
}




void execute_select(const char* current_db, Pager* pager, uint32_t root_page_id, ParsedQuery* query, int client_sock) {
    char net_buffer[65536]; 
    net_buffer[0] = '\0';
    
    if (strlen(current_db) == 0) {
        sprintf(net_buffer, "ERROR|No database selected. Use 'USE <dbname>;' first.\n");
        send(client_sock, net_buffer, strlen(net_buffer), 0);
        return;
    }

    // --- THE SCHEMA CACHE OPTIMIZATION ---
    static char cached_table[256] = "";
    static ColumnDef cached_schema[100];
    static int cached_num_cols = 0;
    static int cached_types[100]; // 1=INT, 2=DECIMAL, 3=DATETIME, 4=VARCHAR

    if (strcmp(cached_table, query->table_name) != 0) {
        cached_num_cols = load_schema(current_db, query->table_name, cached_schema);
        if (cached_num_cols != -1) {
            strcpy(cached_table, query->table_name);
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(cached_schema[i].type, "INT") == 0) cached_types[i] = 1;
                else if (strcasecmp(cached_schema[i].type, "DECIMAL") == 0) cached_types[i] = 2;
                else if (strcasecmp(cached_schema[i].type, "DATETIME") == 0) cached_types[i] = 3;
                else cached_types[i] = 4; 
            }
        }
    }

    if (cached_num_cols == -1) {
        sprintf(net_buffer, "ERROR|Table '%s' does not exist.\n", query->table_name);
        send(client_sock, net_buffer, strlen(net_buffer), 0);
        return;
    }

    // Determine how many columns to actually send (Projection logic)
    int cols_to_send = query->select_column_count == 0 ? cached_num_cols : query->select_column_count;

    // ==========================================
    // THE TRAFFIC COP: Route to B-Tree or Scan
    // ==========================================
    int use_btree = 0;
    if (query->has_where && strcmp(query->where_column, cached_schema[0].name) == 0 && strcmp(query->where_operator, "=") == 0) {
        use_btree = 1; 
    }

    if (use_btree) {
        // ----------------------------------------------------
        // PATH A: EXACT MATCH (Use the O(log N) B-Tree)
        // ----------------------------------------------------
        IndexKey search_key;
        if (cached_types[0] == 1) { search_key.type = 1; search_key.value.int_val = atoi(query->where_value); } 
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
                            if(strcasecmp(query->select_columns[j], cached_schema[i].name) == 0) { selected = 1; break; }
                        }
                    }

                    if (selected) {
                        *ptr++ = '|';
                        int name_len = strlen(cached_schema[i].name);
                        memcpy(ptr, cached_schema[i].name, name_len);
                        ptr += name_len;
                        *ptr++ = '|';

                        if (cached_types[i] == 1) {
                            int32_t val; memcpy(&val, raw_record + offset, 4);
                            ptr += sprintf(ptr, "%d", val); offset += 4;
                        } else if (cached_types[i] == 2) {
                            double val; memcpy(&val, raw_record + offset, 8);
                            ptr += sprintf(ptr, "%g", val); offset += 8; // %g drops trailing zeros!
                        } else if (cached_types[i] == 3) {
                            int64_t val; memcpy(&val, raw_record + offset, 8);
                            ptr += sprintf(ptr, "%lld", (long long)val); offset += 8;
                        } else {
                            uint16_t len; memcpy(&len, raw_record + offset, 2); offset += 2;
                            memcpy(ptr, raw_record + offset, len); ptr += len; offset += len;
                        }
                    } else {
                        // Skip the memory block if not selected!
                        if (cached_types[i] == 1) offset += 4;
                        else if (cached_types[i] == 2) offset += 8;
                        else if (cached_types[i] == 3) offset += 8;
                        else { uint16_t len; memcpy(&len, raw_record + offset, 2); offset += 2 + len; }
                    }
                }
                
                *ptr++ = '\n';
                const char* done_msg = "DONE|Query executed successfully.\n";
                memcpy(ptr, done_msg, strlen(done_msg));
                ptr += strlen(done_msg);
                send(client_sock, net_buffer, ptr - net_buffer, 0);
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
                if (strcasecmp(cached_schema[i].name, query->where_column) == 0) { where_idx = i; break; }
            }
        }

        int order_idx = -1;
        if (query->has_order_by) {
            for (int i = 0; i < cached_num_cols; i++) {
                if (strcasecmp(cached_schema[i].name, query->order_by_column) == 0) { order_idx = i; break; }
            }
        }

        // Allocate a dynamic array to hold our unsorted results
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

                    // Calculate memory offsets for this specific row dynamically
                    uint16_t offsets[100];
                    uint16_t cur_off = sizeof(TupleHeader);
                    for(int i = 0; i < cached_num_cols; i++) {
                        offsets[i] = cur_off;
                        if(cached_types[i] == 1) cur_off += 4;
                        else if(cached_types[i] == 2) cur_off += 8;
                        else if(cached_types[i] == 3) cur_off += 8;
                        else { uint16_t len; memcpy(&len, raw_record + cur_off, 2); cur_off += 2 + len; }
                    }

                    // Check condition
                    int match = 1;
                    if (where_idx != -1) {
                        match = evaluate_condition(cached_types[where_idx], raw_record + offsets[where_idx], query->where_operator, query->where_value);
                    }

                    if (match && match_count < 10000) {
                        char temp_row[2048]; char* ptr = temp_row;
                        ptr += sprintf(ptr, "ROW|%d", cols_to_send);

                        for (int i = 0; i < cached_num_cols; i++) {
                            int selected = (query->select_column_count == 0);
                            if (!selected) {
                                for(int j=0; j<query->select_column_count; j++){
                                    if(strcasecmp(query->select_columns[j], cached_schema[i].name) == 0) { selected = 1; break; }
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
                                    ptr += sprintf(ptr, "%lld", (long long)v);
                                } else {
                                    uint16_t len; memcpy(&len, raw_record + offsets[i], 2);
                                    memcpy(ptr, raw_record + offsets[i] + 2, len); ptr += len;
                                }
                            }
                        }
                        *ptr = '\0'; // Null terminate the string safely
                        results[match_count].row_str = strdup(temp_row);

                        // Extract the Sorting Key
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

        // Sort Data
        if (query->has_order_by && match_count > 0) {
            qsort(results, match_count, sizeof(SortRow), compare_sort_rows);
        }

        // Batch send the rows back to the client!
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