#ifndef SCHEMA_H
#define SCHEMA_H
#include "../parser/parser.h"

// Added 'current_db' parameter
int save_schema(const char* current_db, const char* table_name, ColumnDef* columns, int num_columns);
int load_schema(const char* current_db, const char* table_name, ColumnDef* columns);

#endif