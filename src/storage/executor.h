#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "../parser/parser.h"
#include "pager.h"

// Database operations
int execute_create_db(ParsedQuery* query);
int execute_use_db(ParsedQuery* query, char* current_db_session);
int execute_drop_db(ParsedQuery* query, char* current_db_session); // <-- Changed to int
void execute_show_db(int client_sock);

// Table operations
int execute_create(const char* current_db, ParsedQuery* query);      // <-- Changed to int
int execute_drop_table(const char* current_db, ParsedQuery* query);  // <-- Changed to int
void execute_show_tables(const char* current_db, int client_sock);

// Row operations
int execute_insert(const char* current_db, Pager* pager, uint32_t* root_page_id, uint32_t* active_data_page, ParsedQuery* query, int client_sock);
void execute_select(const char* current_db, Pager* pager, uint32_t root_page_id, ParsedQuery* query, int client_sock);
int execute_delete(const char* current_db, ParsedQuery* query);      // <-- Changed to int

#endif