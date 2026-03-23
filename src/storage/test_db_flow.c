#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pager.h"
#include "btree.h"
#include "executor.h"

int main() {
    printf("==================================================\n");
    printf("   Testing Full Database Lifecycle & Execution    \n");
    printf("==================================================\n");

    // This represents the client's current session state
    char current_db[MAX_DB_NAME_LEN] = ""; 
    ParsedQuery q;

    // --- 1. CREATE DATABASE ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_CREATE_DB;
    strcpy(q.db_name, "test_school");
    printf("\n--- Step 1: CREATE DATABASE ---\n");
    execute_create_db(&q);

    // --- 2. USE DATABASE ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_USE_DB;
    strcpy(q.db_name, "test_school");
    printf("\n--- Step 2: USE DATABASE ---\n");
    execute_use_db(&q, current_db);

    // --- 3. CREATE TABLE ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_CREATE_TABLE;
    strcpy(q.table_name, "STUDENTS");
    q.column_count = 2; 
    strcpy(q.columns[0].name, "ID");    strcpy(q.columns[0].type, "INT");
    strcpy(q.columns[1].name, "NAME");  strcpy(q.columns[1].type, "VARCHAR");
    printf("\n--- Step 3: CREATE TABLE ---\n");
    execute_create(current_db, &q);

    // --- Prepare Pager for the STUDENTS table ---
    char dat_filepath[512];
    snprintf(dat_filepath, sizeof(dat_filepath), "%s/%s.dat", current_db, "STUDENTS");
    Pager* pager = pager_open(dat_filepath);
    
    // Initialize the root page for this specific table
    uint32_t root_page_id = 0; 
    Page* root_page = get_page(pager, root_page_id);
    BTreeNode* root_node = (BTreeNode*)root_page->data;
    root_node->is_leaf = 1;
    root_node->is_root = 1;
    root_node->num_keys = 0;
    unpin_page(pager, root_page_id, 1); 

    // --- 4. INSERT ROW ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_INSERT;
    strcpy(q.table_name, "STUDENTS");
    q.value_count = 2;
    strcpy(q.values[0], "101");
    strcpy(q.values[1], "FlexQL_Student");
    printf("\n--- Step 4: INSERT ROW ---\n");
    execute_insert(current_db, pager, &root_page_id, &q); // <-- Added current_db

    
    
    
    // --- 5. SELECT ROW ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_SELECT;
    strcpy(q.table_name, "STUDENTS");
    strcpy(q.where_value, "101");
    printf("\n--- Step 5: SELECT ROW ---\n");
    execute_select(current_db, pager, root_page_id, &q);  // <-- Added current_db

    // Close the pager before dropping files so we don't corrupt the OS file handles
    pager_close(pager);

    // --- 6. DROP TABLE ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_DROP_TABLE;
    strcpy(q.table_name, "STUDENTS");
    printf("\n--- Step 6: DROP TABLE ---\n");
    execute_drop_table(current_db, &q);

    // --- 7. DROP DATABASE ---
    memset(&q, 0, sizeof(ParsedQuery));
    q.type = CMD_DROP_DB;
    strcpy(q.db_name, "test_school");
    printf("\n--- Step 7: DROP DATABASE ---\n");
    execute_drop_db(&q, current_db);

    printf("\n==================================================\n");
    printf("   Lifecycle Test Complete! All files cleaned up. \n");
    printf("==================================================\n");

    return 0;
}


// gcc src/storage/test_db_flow.c src/storage/executor.c src/storage/schema.c src/storage/btree.c src/storage/pager.c -o test_db_flow -lpthread
// ./test_db_flow