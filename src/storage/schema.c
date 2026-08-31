#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schema.h"

int save_schema(const char* current_db, const char* table_name, ColumnDef* columns, int num_columns) {
    char filename[512];
    // Route into the database directory!
    snprintf(filename, sizeof(filename), "%s/%s.schema", current_db, table_name);

    FILE* file = fopen(filename, "w");
    if (file == NULL) {
        perror("[-] Failed to create schema file");
        return -1;
    }

    for (int i = 0; i < num_columns; i++) {
        fprintf(file, "%s %s %d %d\n", columns[i].name, columns[i].type, columns[i].is_primary_key, columns[i].is_not_null);
    }

    fclose(file);
    return 0;
}

int load_schema(const char* current_db, const char* table_name, ColumnDef* columns) {
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/%s.schema", current_db, table_name);

    FILE* file = fopen(filename, "r");
    if (file == NULL) return -1; 

    int num_columns = 0;
    int is_pk, is_nn; // fscanf requires standard ints for %d
    
    // Read all 4 properties from the disk!
    while (fscanf(file, "%s %s %d %d", columns[num_columns].name, columns[num_columns].type, &is_pk, &is_nn) == 4) {
        columns[num_columns].is_primary_key = is_pk;
        columns[num_columns].is_not_null = is_nn;
        num_columns++;
        if (num_columns >= 100) break;
    }

    fclose(file);
    return num_columns;
}

void save_root_page(const char* current_db, const char* table_name, uint32_t root_page_id) {
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/%s.root", current_db, table_name);

    FILE* file = fopen(filename, "wb");
    if (file == NULL) return;
    fwrite(&root_page_id, sizeof(uint32_t), 1, file);
    fclose(file);
}

uint32_t load_root_page(const char* current_db, const char* table_name) {
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/%s.root", current_db, table_name);

    FILE* file = fopen(filename, "rb");
    if (file == NULL) return 0; // No splits yet (or a freshly created table) - root is still page 0.

    uint32_t root_page_id = 0;
    if (fread(&root_page_id, sizeof(uint32_t), 1, file) != 1) root_page_id = 0;
    fclose(file);
    return root_page_id;
}

void delete_root_page(const char* current_db, const char* table_name) {
    char filename[512];
    snprintf(filename, sizeof(filename), "%s/%s.root", current_db, table_name);
    remove(filename);
}