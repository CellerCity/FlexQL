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


// --- 1. Your Excellent Serialization Logic ---
uint16_t serialize_row(ParsedQuery* query, ColumnDef* schema, char* tuple_buffer) {
    uint16_t current_offset = 0;

    // Mandatory Expiration Timestamp
    TupleHeader header;
    header.expiration_timestamp = (uint64_t)time(NULL) + (30 * 24 * 60 * 60); 
    
    memcpy(tuple_buffer + current_offset, &header, sizeof(TupleHeader));
    current_offset += sizeof(TupleHeader);

    for (int i = 0; i < query->value_count; i++) {
        if (strcasecmp(schema[i].type, "INT") == 0) {
            int32_t val = atoi(query->values[i]);
            memcpy(tuple_buffer + current_offset, &val, sizeof(int32_t));
            current_offset += sizeof(int32_t);
        } else if (strcasecmp(schema[i].type, "DECIMAL") == 0) {
            double val = atof(query->values[i]);
            memcpy(tuple_buffer + current_offset, &val, sizeof(double));
            current_offset += sizeof(double);
        } else if (strcasecmp(schema[i].type, "DATETIME") == 0) {
            struct tm tm_info;
            memset(&tm_info, 0, sizeof(struct tm));
            int64_t timestamp = 0;
            if (strptime(query->values[i], "%Y-%m-%d %H:%M:%S", &tm_info) != NULL ||
                strptime(query->values[i], "%Y-%m-%d", &tm_info) != NULL) {
                timestamp = (int64_t)mktime(&tm_info);
            }
            memcpy(tuple_buffer + current_offset, &timestamp, sizeof(int64_t));
            current_offset += sizeof(int64_t);
        } else if (strcasecmp(schema[i].type, "TEXT") == 0 || strcasecmp(schema[i].type, "VARCHAR") == 0) {
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

// --- 3. NEW: The Thread-Safe Pager Wrapper ---
RecordID append_to_data_page(Pager* pager, const char* tuple_buffer, uint16_t tuple_size) {
    // Try to insert into the last page in the file
    uint32_t data_page_id = pager->num_pages > 0 ? pager->num_pages - 1 : 0;
    Page* page = get_page(pager, data_page_id);

    // If it's an index page or it's full, we need a brand new page
    if (page->header.page_type != 0 || insert_into_page(page, tuple_buffer, tuple_size) == -1) {
        unpin_page(pager, data_page_id, 0); // Release the old page
        
        data_page_id = pager->num_pages; // Grab a fresh page
        page = get_page(pager, data_page_id);
        page->header.page_type = 0; // Mark as DATA
        
        insert_into_page(page, tuple_buffer, tuple_size); 
    }

    uint16_t slot_num = page->header.num_slots - 1; // Get the slot we just created
    unpin_page(pager, data_page_id, 1); // Done writing, mark as dirty!

    RecordID rec = { .page_num = data_page_id, .slot_num = slot_num };
    return rec;
}

// =========================================================
//                   THE MAIN EXECUTOR API
// =========================================================

void execute_create(const char* current_db, ParsedQuery* query) {
    if (strlen(current_db) == 0) {
        printf("[-] Error: No database selected. Use 'USE <dbname>;' first.\n");
        return;
    }

    if (save_schema(current_db, query->table_name, query->columns, query->column_count) == 0) {
        printf("[+] Table '%s' created in database '%s'.\n", query->table_name, current_db);
    }
}

void execute_insert(const char* current_db, Pager* pager, uint32_t* root_page_id, ParsedQuery* query) {
    if (strlen(current_db) == 0) {
        printf("[-] Error: No database selected. Use 'USE <dbname>;' first.\n");
        return;
    }
    
    ColumnDef schema[100];
    int num_cols = load_schema(current_db, query->table_name, schema);
    
    if (num_cols == -1) {
        printf("[-] Error: Table '%s' does not exist.\n", query->table_name);
        return;
    }

    // 1. Serialize row
    char tuple_buffer[MAX_TUPLE_SIZE];
    uint16_t tuple_size = serialize_row(query, schema, tuple_buffer);

    // 2. Drop into Data Page
    RecordID record_location = append_to_data_page(pager, tuple_buffer, tuple_size);

    // 3. Extract Primary Key and link to B+ Tree
    IndexKey pk;
    if (strcasecmp(schema[0].type, "INT") == 0) {
        pk.type = 1;
        pk.value.int_val = atoi(query->values[0]);
    } else if (strcasecmp(schema[0].type, "VARCHAR") == 0) {
        pk.type = 4;
        strncpy(pk.value.str_val, query->values[0], 32);
    }
    // Note: You can add DECIMAL/DATETIME PK extraction here based on your schema

    btree_insert(pager, root_page_id, pk, record_location);
    printf("[+] Inserted row into '%s' at Page %u, Slot %u.\n", 
           query->table_name, record_location.page_num, record_location.slot_num);
}

void execute_select(const char* current_db, Pager* pager, uint32_t root_page_id, ParsedQuery* query) {
    if (strlen(current_db) == 0) {
        printf("[-] Error: No database selected. Use 'USE <dbname>;' first.\n");
        return;
    }
    
    ColumnDef schema[100];
    if (load_schema(current_db, query->table_name, schema) == -1) {
        printf("[-] Error: Table '%s' does not exist.\n", query->table_name);
        return;
    }

    // 1. Setup Search Key
    IndexKey search_key;
    if (strcasecmp(schema[0].type, "INT") == 0) {
        search_key.type = 1;
        search_key.value.int_val = atoi(query->where_value); 
    } else {
        search_key.type = 4;
        strncpy(search_key.value.str_val, query->where_value, 32);
    }

    RecordID result;
    
    // 2. Search Tree
    if (btree_search(pager, root_page_id, search_key, &result)) {
        
        Page* data_page = get_page(pager, result.page_num);
        Slot* slots = (Slot*)data_page->data;
        Slot my_slot = slots[result.slot_num];
        
        char* raw_record = &data_page->data[my_slot.offset - sizeof(PageHeader)];
        
        // 3. Check Expiration
        TupleHeader header;
        memcpy(&header, raw_record, sizeof(TupleHeader));
        
        if (header.expiration_timestamp < (uint64_t)time(NULL)) {
            printf("[-] Record found, but it has EXPIRED.\n");
        } else {
            printf("[+] Record Found! Active until timestamp: %lu\n", header.expiration_timestamp);
            // Future step: deserialize raw_record back into columns here using the schema!
        }
        
        unpin_page(pager, result.page_num, 0); 
    } else {
        printf("[-] Record '%s' not found.\n", query->where_value);
    }
}



