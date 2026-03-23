#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "../parser/parser.h"
#include "pager.h"

// Database operations
int execute_create_db(ParsedQuery* query);
int execute_use_db(ParsedQuery* query, char* current_db_session);
void execute_drop_db(ParsedQuery* query, char* current_db_session);

// Table operations
void execute_create(const char* current_db, ParsedQuery* query);
void execute_drop_table(const char* current_db, ParsedQuery* query);

// Row operations
void execute_insert(const char* current_db, Pager* pager, uint32_t* root_page_id, ParsedQuery* query);
void execute_select(const char* current_db, Pager* pager, uint32_t root_page_id, ParsedQuery* query);

#endif