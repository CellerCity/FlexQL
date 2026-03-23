#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schema.h"

// --- Save Schema to Disk ---
int save_schema(const char* table_name, ColumnDef* columns, int num_columns) {
    // 1. Create the filename (e.g., "STUDENT.schema")
    char filename[256];
    snprintf(filename, sizeof(filename), "%s.schema", table_name);

    // 2. Open the file for writing ("w" creates it or overwrites it)
    FILE* file = fopen(filename, "w");
    if (file == NULL) {
        perror("[-] Failed to create schema file");
        return -1;
    }

    // 3. Write each column definition line by line
    for (int i = 0; i < num_columns; i++) {
        fprintf(file, "%s %s\n", columns[i].name, columns[i].type);
    }

    // 4. Close the file safely
    fclose(file);
    return 0;
}

// --- Load Schema from Disk ---
int load_schema(const char* table_name, ColumnDef* columns) {
    char filename[256];
    snprintf(filename, sizeof(filename), "%s.schema", table_name);

    // 1. Open the file for reading ("r")
    FILE* file = fopen(filename, "r");
    if (file == NULL) {
        // This is perfectly normal if the user tries to SELECT from a table that doesn't exist
        return -1; 
    }

    int num_columns = 0;
    
    // 2. Read the file line by line using fscanf
    // It expects a string (name), a space, and another string (type)
    while (fscanf(file, "%s %s", columns[num_columns].name, columns[num_columns].type) == 2) {
        num_columns++;
        
        // Safety check to prevent buffer overflows if a table has too many columns
        if (num_columns >= 100) { 
            break;
        }
    }

    // 3. Close the file and return the count
    fclose(file);
    return num_columns;
}