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
        fprintf(file, "%s %s\n", columns[i].name, columns[i].type);
    }

    fclose(file);
    return 0;
}

int load_schema(const char* current_db, const char* table_name, ColumnDef* columns) {
    char filename[512];
    // Route into the database directory!
    snprintf(filename, sizeof(filename), "%s/%s.schema", current_db, table_name);

    FILE* file = fopen(filename, "r");
    if (file == NULL) return -1; 

    int num_columns = 0;
    while (fscanf(file, "%s %s", columns[num_columns].name, columns[num_columns].type) == 2) {
        num_columns++;
        if (num_columns >= 100) break;
    }

    fclose(file);
    return num_columns;
}