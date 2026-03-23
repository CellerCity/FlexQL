#ifndef SCHEMA_H
#define SCHEMA_H

#include "../parser/parser.h" // Ensures we have access to the ColumnDef struct

// --- Function Prototypes ---

// Saves a table's schema to a physical file (e.g., "STUDENT.schema")
// Returns 0 on success, -1 on failure.
int save_schema(const char* table_name, ColumnDef* columns, int num_columns);

// Loads a table's schema from disk into the provided array.
// Returns the number of columns loaded, or -1 if the table does not exist.
int load_schema(const char* table_name, ColumnDef* columns);

#endif