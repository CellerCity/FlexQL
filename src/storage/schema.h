#ifndef SCHEMA_H
#define SCHEMA_H
#include <stdint.h>
#include "../parser/parser.h"

// Added 'current_db' parameter
int save_schema(const char* current_db, const char* table_name, ColumnDef* columns, int num_columns);
int load_schema(const char* current_db, const char* table_name, ColumnDef* columns);

// The B+Tree root page moves off page 0 the first time it splits. Every new
// connection (and every table switch within a connection) previously assumed
// root_page_id == 0, which silently broke PK lookups/uniqueness checks past
// the first split. These persist the current root page id to a small
// sidecar file so any fresh connection can pick up where the tree left off.
void save_root_page(const char* current_db, const char* table_name, uint32_t root_page_id);
uint32_t load_root_page(const char* current_db, const char* table_name);
void delete_root_page(const char* current_db, const char* table_name);

#endif