#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "parser.h"

// Helper function to remove leading/trailing whitespace and quotes
void trim_string(char *str) {
    if (!str) return;
    char *start = str;
    while (isspace((unsigned char)*start) || *start == '\'' || *start == '"') start++;
    char *end = start + strlen(start) - 1;
    while (end > start && (isspace((unsigned char)*end) || *end == '\'' || *end == '"' || *end == ';')) end--;
    *(end + 1) = '\0';
    memmove(str, start, strlen(start) + 1);
}

// =========================================================
// MODULAR PARSERS
// =========================================================

static void parse_use(char* saveptr, ParsedQuery* query) {
    char* token = strtok_r(NULL, " \t\n;", &saveptr);
    if (token) {
        query->type = CMD_USE_DB;
        strcpy(query->db_name, token);
        query->is_valid = 1;
    } else {
        strcpy(query->error_msg, "Syntax error: Expected database name after USE.");
    }
}

static void parse_create(char* saveptr, const char* sql_string, ParsedQuery* query) {
    char* token = strtok_r(NULL, " \t\n", &saveptr);
    if (!token) {
        strcpy(query->error_msg, "Syntax error: Expected DATABASE or TABLE after CREATE.");
        return;
    }

    if (strcasecmp(token, "DATABASE") == 0) {
        token = strtok_r(NULL, " \t\n;", &saveptr);
        if (token) {
            query->type = CMD_CREATE_DB;
            strcpy(query->db_name, token);
            query->is_valid = 1;
        } else {
            strcpy(query->error_msg, "Syntax error: Expected database name.");
        }
    } 
    else if (strcasecmp(token, "TABLE") == 0) {
        query->type = CMD_CREATE_TABLE;
        
        token = strtok_r(NULL, " \t\n(", &saveptr);
        if (token && strcasecmp(token, "IF") == 0) {
            strtok_r(NULL, " \t\n", &saveptr); // skip NOT
            strtok_r(NULL, " \t\n", &saveptr); // skip EXISTS
            token = strtok_r(NULL, " \t\n(", &saveptr);
        }

        if (token) {
            strcpy(query->table_name, token);
            const char *start_paren = strchr(sql_string, '(');
            const char *end_paren = strrchr(sql_string, ')');
            
            if (start_paren && end_paren && start_paren < end_paren) {
                char cols_str[512];
                int len = end_paren - start_paren - 1;
                if (len >= 512) len = 511; // Safety
                strncpy(cols_str, start_paren + 1, len);
                cols_str[len] = '\0';
                
                // DEFENSE: Check for empty parentheses CREATE TABLE empty ()
                char check_empty[512];
                strcpy(check_empty, cols_str);
                trim_string(check_empty);
                if (strlen(check_empty) == 0) {
                    strcpy(query->error_msg, "Syntax error: Table must have at least one column.");
                    return;
                }
                
                query->is_valid = 1; 
                int pk_count = 0; 
                char *saveptr_comma;
                char *col_token = strtok_r(cols_str, ",", &saveptr_comma);
                
                while (col_token && query->column_count < MAX_COLUMNS && query->is_valid) {
                    trim_string(col_token);
                    char col_copy[128];
                    strncpy(col_copy, col_token, sizeof(col_copy)-1);
                    col_copy[sizeof(col_copy)-1] = '\0';
                    
                    char *saveptr_space;
                    char *word = strtok_r(col_copy, " \t", &saveptr_space);
                    if (word) {
                        strcpy(query->columns[query->column_count].name, word); 
                        word = strtok_r(NULL, " \t", &saveptr_space);
                        
                        if (word) {
                            if (strcasecmp(word, "INT") != 0 &&
                                strcasecmp(word, "DECIMAL") != 0 &&
                                strncasecmp(word, "VARCHAR", 7) != 0 && 
                                strcasecmp(word, "TEXT") != 0 && 
                                strcasecmp(word, "DATETIME") != 0) {
                                snprintf(query->error_msg, sizeof(query->error_msg), "Syntax error: Invalid type '%s'.", word);
                                query->is_valid = 0; break;
                            }
                            
                            if (strncasecmp(word, "VARCHAR", 7) == 0) strcpy(query->columns[query->column_count].type, "VARCHAR");
                            else strcpy(query->columns[query->column_count].type, word); 
                            
                            while ((word = strtok_r(NULL, " \t", &saveptr_space)) != NULL) {
                                if (strcasecmp(word, "PRIMARY") == 0) {
                                    query->columns[query->column_count].is_primary_key = 1;
                                    pk_count++;
                                } else if (strcasecmp(word, "NOT") == 0) {
                                    query->columns[query->column_count].is_not_null = 1;
                                }
                            }
                            
                            if (pk_count > 1) {
                                strcpy(query->error_msg, "Syntax error: Only one PRIMARY KEY allowed.");
                                query->is_valid = 0; break;
                            }
                            query->column_count++;
                        } else {
                            strcpy(query->error_msg, "Syntax error: Missing data type.");
                            query->is_valid = 0; break;
                        }
                    }
                    col_token = strtok_r(NULL, ",", &saveptr_comma);
                }
            } else {
                strcpy(query->error_msg, "Syntax error: Missing parentheses.");
            }
        } else {
            strcpy(query->error_msg, "Syntax error: Expected table name.");
        }
    } else {
        strcpy(query->error_msg, "Syntax error: Expected DATABASE or TABLE after CREATE.");
    }
}

static void parse_drop(char* saveptr, ParsedQuery* query) {
    char* token = strtok_r(NULL, " \t\n", &saveptr);
    if (!token) {
        strcpy(query->error_msg, "Syntax error: Expected DATABASE or TABLE after DROP.");
        return;
    }
    if (strcasecmp(token, "TABLE") == 0) {
        token = strtok_r(NULL, " \t\n;", &saveptr);
        if (token) {
            query->type = CMD_DROP_TABLE;
            strcpy(query->table_name, token);
            query->is_valid = 1;
        } else {
             strcpy(query->error_msg, "Syntax error: Expected table name.");
        }
    } else if (strcasecmp(token, "DATABASE") == 0) {
         token = strtok_r(NULL, " \t\n;", &saveptr);
         if (token) {
             query->type = CMD_DROP_DB;
             strcpy(query->db_name, token);
             query->is_valid = 1;
         } else {
             strcpy(query->error_msg, "Syntax error: Expected database name.");
         }
    } else {
         strcpy(query->error_msg, "Syntax error: Expected DATABASE or TABLE after DROP.");
    }
}

static void parse_insert(char* saveptr, const char* sql_string, ParsedQuery* query) {
    query->type = CMD_INSERT;
    char* token = strtok_r(NULL, " \t\n", &saveptr); 
    
    if (token && strcasecmp(token, "INTO") == 0) {
        token = strtok_r(NULL, " \t\n(", &saveptr); 
        if (token) {
            strcpy(query->table_name, token);
            token = strtok_r(NULL, " \t\n(", &saveptr); 
            if (token && strcasecmp(token, "VALUES") == 0) {

                const char *start_paren = strchr(sql_string, '(');
                if (start_paren) {
                    int open_count = 0, close_count = 0, in_string = 0;
                    int empty_check = 1; // DEFENSE: Check for empty parens
                    const char* scan_ptr = start_paren;
                    
                    while (*scan_ptr != '\0' && *scan_ptr != ';') {
                        if (*scan_ptr == '\'' || *scan_ptr == '"') {
                            in_string = !in_string; 
                            empty_check = 0;
                        }
                        if (!in_string) {
                            if (*scan_ptr == '(') open_count++;
                            else if (*scan_ptr == ')') close_count++;
                            else if (!isspace((unsigned char)*scan_ptr) && *scan_ptr != ',') {
                                empty_check = 0; // It has actual data!
                            }
                        }
                        scan_ptr++;
                    }
                    
                    if (open_count == 0 || open_count != close_count || in_string) {
                        strcpy(query->error_msg, "Syntax error: Malformed or incomplete tuple list.");
                    } else if (empty_check) {
                        strcpy(query->error_msg, "Syntax error: VALUES cannot be empty.");
                    } else {
                        query->bulk_insert_ptr = strdup(start_paren);
                        query->is_valid = 1;
                    }
                } else {
                    strcpy(query->error_msg, "Syntax error: Missing parentheses in VALUES clause.");
                }
            } else {
                 strcpy(query->error_msg, "Syntax error: Expected VALUES keyword.");
            }
        } else {
             strcpy(query->error_msg, "Syntax error: Expected table name.");
        }
    } else {
         strcpy(query->error_msg, "Syntax error: Expected INTO after INSERT.");
    }
}

static void parse_delete(char* saveptr, ParsedQuery* query) {
    query->type = CMD_DELETE;
    char* token = strtok_r(NULL, " \t\n", &saveptr);
    if (token && strcasecmp(token, "FROM") == 0) {
        token = strtok_r(NULL, " \t\n;", &saveptr);
        if (token) {
            strcpy(query->table_name, token);
            query->is_valid = 1;
        } else {
            strcpy(query->error_msg, "Syntax error: Expected table name after FROM.");
        }
    } else {
        strcpy(query->error_msg, "Syntax error: Expected FROM after DELETE.");
    }
}

static void parse_select(char* saveptr, ParsedQuery* query) {
    query->type = CMD_SELECT;
    query->is_valid = 1; 
    int state = 1; 

    char* token;
    while ((token = strtok_r(NULL, " \t\n;", &saveptr)) != NULL) {
        if (strcasecmp(token, "FROM") == 0) {
            state = 2;
            token = strtok_r(NULL, " \t\n;", &saveptr);
            if (token) strcpy(query->table_name, token);
            continue;
        }
        if (strcasecmp(token, "INNER") == 0) {
            state = 3;
            token = strtok_r(NULL, " \t\n;", &saveptr); 
            if (token && strcasecmp(token, "JOIN") == 0) {
                query->has_join = 1;
                token = strtok_r(NULL, " \t\n;", &saveptr); 
                if (token) strcpy(query->join_table, token);
                token = strtok_r(NULL, " \t\n;", &saveptr); // ON
                token = strtok_r(NULL, " \t\n;", &saveptr); // L-Cond
                if (token) strcpy(query->join_condition_left, token);
                token = strtok_r(NULL, " \t\n;", &saveptr); // =
                token = strtok_r(NULL, " \t\n;", &saveptr); // R-Cond
                if (token) strcpy(query->join_condition_right, token);
            }
            continue;
        }
        if (strcasecmp(token, "WHERE") == 0) {
            state = 4;
            query->has_where = 1;
            token = strtok_r(NULL, " \t\n;", &saveptr);
            if (token) strcpy(query->where_column, token);
            
            token = strtok_r(NULL, " \t\n;", &saveptr);
            if (token) {
                // DEFENSE: Strictly validate the operator
                if (strcmp(token, "=") == 0 || strcmp(token, ">") == 0 || strcmp(token, "<") == 0 ||
                    strcmp(token, ">=") == 0 || strcmp(token, "<=") == 0) {
                    strcpy(query->where_operator, token);
                } else {
                    strcpy(query->error_msg, "Syntax error: Invalid WHERE operator.");
                    query->is_valid = 0;
                    return;
                }
            }
            
            token = strtok_r(NULL, " \t\n;", &saveptr);
            if (token) {
                trim_string(token); 
                strcpy(query->where_value, token);
            }
            continue;
        }
        if (strcasecmp(token, "ORDER") == 0) {
            state = 5;
            token = strtok_r(NULL, " \t\n;", &saveptr); // BY
            if (token && strcasecmp(token, "BY") == 0) {
                query->has_order_by = 1;
                token = strtok_r(NULL, " \t\n;", &saveptr); // Col
                if (token) strcpy(query->order_by_column, token);
            }
            continue;
        }
        
        // ASC / DESC check
        if (state == 5) {
            if (strcasecmp(token, "DESC") == 0) query->order_by_desc = 1;
            else if (strcasecmp(token, "ASC") == 0) query->order_by_desc = 0;
            state = 6;
            continue;
        }

        if (state == 1) {
            if (token[strlen(token) - 1] == ',') token[strlen(token) - 1] = '\0';
            if (strcmp(token, "*") == 0) query->select_column_count = 0; 
            else if (strlen(token) > 0) {
                strcpy(query->select_columns[query->select_column_count], token);
                query->select_column_count++;
            }
        }
    }
    
    if (strlen(query->table_name) == 0) {
        strcpy(query->error_msg, "Syntax error: Missing FROM clause or table name.");
        query->is_valid = 0;
    }
}

static void parse_show(char* saveptr, ParsedQuery* query) {
    char* token = strtok_r(NULL, " \t\n;", &saveptr);
    if (!token) {
        strcpy(query->error_msg, "Syntax error: Expected DATABASES or TABLES after SHOW.");
        return;
    }
    if (strcasecmp(token, "DATABASES") == 0) {
        query->type = CMD_SHOW_DB;
        query->is_valid = 1;
    } else if (strcasecmp(token, "TABLES") == 0) {
        query->type = CMD_SHOW_TABLES;
        query->is_valid = 1;
    } else {
        strcpy(query->error_msg, "Syntax error: Unsupported SHOW command.");
    }
}


// =========================================================
// MAIN ENTRY POINT
// =========================================================
ParsedQuery parse_sql(const char* sql_string) {
    ParsedQuery query;

    // Targeted Resets
    query.type = CMD_UNKNOWN;
    query.is_valid = 0;
    query.column_count = 0;
    query.value_count = 0;
    query.select_column_count = 0;
    query.has_join = 0;
    query.has_where = 0;
    query.has_order_by = 0;
    query.order_by_desc = 0; // Default ASC
    
    query.table_name[0] = '\0';
    query.db_name[0] = '\0';
    query.error_msg[0] = '\0';
    query.where_column[0] = '\0';
    query.where_operator[0] = '\0';
    query.where_value[0] = '\0';
    query.join_table[0] = '\0';
    query.join_condition_left[0] = '\0';
    query.join_condition_right[0] = '\0';
    query.order_by_column[0] = '\0';
    query.bulk_insert_ptr = NULL;

    char* sql_copy = strdup(sql_string);
    char* saveptr;
    
    char *token = strtok_r(sql_copy, " \t\n", &saveptr);
    if (!token) {
        strcpy(query.error_msg, "Empty query.");
        free(sql_copy);
        return query;
    }

    // THE ROUTER
    if (strcasecmp(token, "USE") == 0) parse_use(saveptr, &query);
    else if (strcasecmp(token, "CREATE") == 0) parse_create(saveptr, sql_string, &query);
    else if (strcasecmp(token, "DROP") == 0) parse_drop(saveptr, &query);
    else if (strcasecmp(token, "INSERT") == 0) parse_insert(saveptr, sql_string, &query);
    else if (strcasecmp(token, "DELETE") == 0) parse_delete(saveptr, &query);
    else if (strcasecmp(token, "SELECT") == 0) parse_select(saveptr, &query);
    else if (strcasecmp(token, "SHOW") == 0) parse_show(saveptr, &query);
    else strcpy(query.error_msg, "Unsupported or unknown command.");

    free(sql_copy);
    return query;
}