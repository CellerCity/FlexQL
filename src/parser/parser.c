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
    // Strip trailing spaces, quotes, and semicolons
    while (end > start && (isspace((unsigned char)*end) || *end == '\'' || *end == '"' || *end == ';')) end--;
    
    *(end + 1) = '\0';
    memmove(str, start, strlen(start) + 1);
}

ParsedQuery parse_sql(const char* sql_string) {
    ParsedQuery query;


    // --- THE ARENA OPTIMIZATION ---
    // REMOVE THIS LINE:
    // memset(&query, 0, sizeof(ParsedQuery)); 
    
    // REPLACE WITH TARGETED RESETS:
    query.type = CMD_UNKNOWN;
    query.is_valid = 0;
    
    // Reset Counters
    query.column_count = 0;
    query.value_count = 0;
    query.select_column_count = 0;
    
    // Reset Flags
    query.has_join = 0;
    query.has_where = 0;
    
    // Null-terminate the strings (This takes 1 byte of writing instead of 256 bytes!)
    query.table_name[0] = '\0';
    query.db_name[0] = '\0';
    query.error_msg[0] = '\0';
    query.where_column[0] = '\0';
    query.where_operator[0] = '\0';
    query.where_value[0] = '\0';
    query.join_table[0] = '\0';
    query.join_condition_left[0] = '\0';
    query.join_condition_right[0] = '\0';

    query.bulk_insert_ptr = NULL;
   
    query.has_order_by = 0;
    query.order_by_column[0] = '\0';
    // ------------------------------

    // Make a working copy of the string (strtok modifies the original)
    // Dynamic allocation to prevent stack overflow!
    char* sql_copy = strdup(sql_string);


    // Extract the first keyword
    char *token = strtok(sql_copy, " \t\n");
    if (!token) {
        strcpy(query.error_msg, "Empty query.");
        free(sql_copy);
        return query;
    }

    // =========================================================
    // 1. USE COMMAND
    // Syntax: USE database_name;
    // =========================================================
    if (strcasecmp(token, "USE") == 0) {
        token = strtok(NULL, " \t\n;");
        if (token) {
            query.type = CMD_USE_DB;
            strcpy(query.db_name, token);
            query.is_valid = 1;
        } else {
            strcpy(query.error_msg, "Syntax error: Expected database name after USE.");
        }
    }
    // =========================================================
    // 2. CREATE COMMANDS
    // Syntax: CREATE DATABASE db_name;
    // Syntax: CREATE TABLE table_name (...);
    // =========================================================
    else if (strcasecmp(token, "CREATE") == 0) {
        
        // Get the next word (should be "DATABASE" or "TABLE")
        token = strtok(NULL, " \t\n"); 
        
        if (token && strcasecmp(token, "DATABASE") == 0) {
            // It's a CREATE DATABASE command!
            token = strtok(NULL, " \t\n;");
            if (token) {
                query.type = CMD_CREATE_DB;
                strcpy(query.db_name, token);
                query.is_valid = 1;
            } else {
                strcpy(query.error_msg, "Syntax error: Expected database name.");
            }
        } 
        else if (token && strcasecmp(token, "TABLE") == 0) {
            query.type = CMD_CREATE_TABLE;
            
            // --- TRAP 1: Bypass "IF NOT EXISTS" ---
            token = strtok(NULL, " \t\n(");
            if (token && strcasecmp(token, "IF") == 0) {
                strtok(NULL, " \t\n"); // skip "NOT"
                strtok(NULL, " \t\n"); // skip "EXISTS"
                token = strtok(NULL, " \t\n("); // Get the actual table name
            }

            if (token) {
                strcpy(query.table_name, token);
                
                const char *start_paren = strchr(sql_string, '(');
                const char *end_paren = strrchr(sql_string, ')');
                
                if (start_paren && end_paren && start_paren < end_paren) {
                    char cols_str[512];
                    int len = end_paren - start_paren - 1;
                    strncpy(cols_str, start_paren + 1, len);
                    cols_str[len] = '\0';
                    
                    int pk_count = 0; 
                    query.is_valid = 1; 
                    
                    char *col_token = strtok(cols_str, ",");
                    while (col_token && query.column_count < MAX_COLUMNS && query.is_valid) {
                        trim_string(col_token);
                        
                        char col_copy[128];
                        strncpy(col_copy, col_token, sizeof(col_copy)-1);
                        
                        char *word = strtok(col_copy, " \t");
                        if (word) {
                            strcpy(query.columns[query.column_count].name, word); 
                            word = strtok(NULL, " \t");
                            if (word) {
                                
                                // --- TRAP 2: Use strncasecmp for VARCHAR(64) ---
                                if (strcasecmp(word, "INT") != 0 &&
                                    strcasecmp(word, "DECIMAL") != 0 &&
                                    strncasecmp(word, "VARCHAR", 7) != 0 && // Only check the first 7 letters!
                                    strcasecmp(word, "TEXT") != 0 && 
                                    strcasecmp(word, "DATETIME") != 0) {
                                    
                                    snprintf(query.error_msg, sizeof(query.error_msg), "Syntax error: Invalid type '%s'.", word);
                                    query.is_valid = 0;
                                    break;
                                }
                                
                                // Force it to save as exactly "VARCHAR" to keep our Executor fast!
                                if (strncasecmp(word, "VARCHAR", 7) == 0) {
                                    strcpy(query.columns[query.column_count].type, "VARCHAR");
                                } else {
                                    strcpy(query.columns[query.column_count].type, word); 
                                }
                                
                                while ((word = strtok(NULL, " \t")) != NULL) {
                                    if (strcasecmp(word, "PRIMARY") == 0) {
                                        query.columns[query.column_count].is_primary_key = 1;
                                        pk_count++;
                                    } else if (strcasecmp(word, "NOT") == 0) {
                                        query.columns[query.column_count].is_not_null = 1;
                                    }
                                }
                                
                                if (pk_count > 1) {
                                    strcpy(query.error_msg, "Syntax error: Only one PRIMARY KEY allowed.");
                                    query.is_valid = 0;
                                    break;
                                }
                                
                                query.column_count++;
                            } else {
                                strcpy(query.error_msg, "Syntax error: Missing data type.");
                                query.is_valid = 0;
                                break;
                            }
                        }
                        col_token = strtok(NULL, ",");
                    }
                } else {
                    strcpy(query.error_msg, "Syntax error: Missing parentheses.");
                    query.is_valid = 0;
                }
            }
        } else {
            strcpy(query.error_msg, "Syntax error: Expected DATABASE or TABLE after CREATE.");
        }
    }
    // =========================================================
    // 3. DROP COMMANDS
    // Syntax: DROP DATABASE db_name;
    // Syntax: DROP TABLE table_name;
    // =========================================================
    else if (strcasecmp(token, "DROP") == 0) {
        token = strtok(NULL, " \t\n");
        if (token && strcasecmp(token, "TABLE") == 0) {
            token = strtok(NULL, " \t\n;");
            if (token) {
                query.type = CMD_DROP_TABLE;
                strcpy(query.table_name, token);
                query.is_valid = 1;
            } else {
                 strcpy(query.error_msg, "Syntax error: Expected table name.");
            }
        } else if (token && strcasecmp(token, "DATABASE") == 0) {
             token = strtok(NULL, " \t\n;");
             if (token) {
                 query.type = CMD_DROP_DB;
                 strcpy(query.db_name, token);
                 query.is_valid = 1;
             } else {
                 strcpy(query.error_msg, "Syntax error: Expected database name.");
             }
        } else {
             strcpy(query.error_msg, "Syntax error: Expected DATABASE or TABLE after DROP.");
        }
    }
    // =========================================================
    // 4. INSERT COMMAND
    // Syntax: INSERT INTO table_name VALUES (...), (...);
    // =========================================================
    else if (strcasecmp(token, "INSERT") == 0) {
        query.type = CMD_INSERT;
        token = strtok(NULL, " \t\n"); // Should be "INTO"
        
        if (token && strcasecmp(token, "INTO") == 0) {
            token = strtok(NULL, " \t\n("); // Should be table_name
            
            if (token) {
                strcpy(query.table_name, token);
                
                token = strtok(NULL, " \t\n("); // Should be "VALUES"
                if (token && strcasecmp(token, "VALUES") == 0) {

                    // --- BULK INSERT OPTIMIZATION ---
                    // Find the very first '(' in the raw, unmodified SQL string
                    const char *start_paren = strchr(sql_string, '(');
                    
                    if (start_paren) {
                        // 1. FAST PRE-FLIGHT CHECK
                        int open_count = 0;
                        int close_count = 0;
                        int in_string = 0;
                        const char* scan_ptr = start_paren;
                        
                        while (*scan_ptr != '\0' && *scan_ptr != ';') {
                            // Ignore parens that are inside SQL strings (e.g., 'Hello (World)')
                            if (*scan_ptr == '\'' || *scan_ptr == '"') {
                                in_string = !in_string; 
                            }
                            
                            if (!in_string) {
                                if (*scan_ptr == '(') open_count++;
                                if (*scan_ptr == ')') close_count++;
                            }
                            scan_ptr++;
                        }
                        
                        // 2. ENFORCE ATOMICITY
                        // If parens don't match, or an open string was never closed, it's a corrupt batch!
                        if (open_count == 0 || open_count != close_count || in_string) {
                            strcpy(query.error_msg, "Syntax error: Malformed or incomplete tuple list in VALUES.");
                            query.is_valid = 0;
                        } else {
                            // 3. IT IS SAFE. Hand it to the executor.
                            query.bulk_insert_ptr = strdup(start_paren);
                            query.is_valid = 1;
                        }
                        
                    } else {
                        strcpy(query.error_msg, "Syntax error: Missing parentheses in VALUES clause.");
                        query.is_valid = 0;
                    }
                    
                    // NOTICE: WE DELETED ALL THE OLD strtok() LOGIC HERE!
                    // The executor.c handles breaking this string into individual rows now.
                    
                } else {
                     strcpy(query.error_msg, "Syntax error: Expected VALUES keyword.");
                     query.is_valid = 0;
                }
            } else {
                 strcpy(query.error_msg, "Syntax error: Expected table name.");
                 query.is_valid = 0;
            }
        } else {
             strcpy(query.error_msg, "Syntax error: Expected INTO after INSERT.");
             query.is_valid = 0;
        }
    }
    // =========================================================
    // 4.5 DELETE COMMAND (TRUNCATE)
    // Syntax: DELETE FROM table_name;
    // =========================================================
    else if (strcasecmp(token, "DELETE") == 0) {
        query.type = CMD_DELETE;
        token = strtok(NULL, " \t\n"); // Should be "FROM"
        
        if (token && strcasecmp(token, "FROM") == 0) {
            token = strtok(NULL, " \t\n;"); // Should be table_name
            if (token) {
                strcpy(query.table_name, token);
                query.is_valid = 1;
            } else {
                strcpy(query.error_msg, "Syntax error: Expected table name after FROM.");
            }
        } else {
            strcpy(query.error_msg, "Syntax error: Expected FROM after DELETE.");
        }
    }
    // =========================================================
    // 5. SELECT COMMAND
    // Syntax: SELECT col1, col2 FROM tableA [INNER JOIN tableB ON A.c1 = B.c2] [WHERE col = val];
    // =========================================================
    else if (strcasecmp(token, "SELECT") == 0) {
        query.type = CMD_SELECT;
        query.is_valid = 1; 
        
        // State tracking: 1=Columns, 2=From, 3=Join, 4=Where, 5=Order By
        int state = 1; 

        while ((token = strtok(NULL, " \t\n;")) != NULL) {
            
            // --- State Triggers ---
            if (strcasecmp(token, "FROM") == 0) {
                state = 2;
                token = strtok(NULL, " \t\n;");
                if (token) strcpy(query.table_name, token);
                continue;
            }
            if (strcasecmp(token, "INNER") == 0) {
                state = 3;
                token = strtok(NULL, " \t\n;"); // Should be "JOIN"
                if (token && strcasecmp(token, "JOIN") == 0) {
                    query.has_join = 1;
                    
                    token = strtok(NULL, " \t\n;"); // Table B
                    if (token) strcpy(query.join_table, token);
                    
                    token = strtok(NULL, " \t\n;"); // Should be "ON"
                    token = strtok(NULL, " \t\n;"); // Left condition (e.g., A.id)
                    if (token) strcpy(query.join_condition_left, token);
                    
                    token = strtok(NULL, " \t\n;"); // Should be "="
                    // We skip storing the "=" for the join since it's always an equality check
                    
                    token = strtok(NULL, " \t\n;"); // Right condition (e.g., B.id)
                    if (token) strcpy(query.join_condition_right, token);
                }
                continue;
            }
            if (strcasecmp(token, "WHERE") == 0) {
                state = 4;
                query.has_where = 1;
                
                token = strtok(NULL, " \t\n;"); // Column
                if (token) strcpy(query.where_column, token);
                
                token = strtok(NULL, " \t\n;"); // Operator (=, >, <, >=, <=)
                if (token) strcpy(query.where_operator, token);
                
                token = strtok(NULL, " \t\n;"); // Value
                if (token) {
                    trim_string(token); // Remove quotes if it's a string like 'Alice'
                    strcpy(query.where_value, token);
                }
                continue;
            }
            
            // --- THE NEW ORDER BY DETECTOR ---
            if (strcasecmp(token, "ORDER") == 0) {
                state = 5;
                token = strtok(NULL, " \t\n;"); // Should be "BY"
                if (token && strcasecmp(token, "BY") == 0) {
                    query.has_order_by = 1;
                    token = strtok(NULL, " \t\n;"); // Column Name
                    if (token) strcpy(query.order_by_column, token);
                }
                continue;
            }


            // --- Column Extraction (State 1) ---
            if (state == 1) {
                // Strip comma if present (e.g., "id," -> "id")
                if (token[strlen(token) - 1] == ',') {
                    token[strlen(token) - 1] = '\0';
                }
                
                if (strcmp(token, "*") == 0) {
                    query.select_column_count = 0; // 0 is our flag for SELECT *
                } else if (strlen(token) > 0) {
                    strcpy(query.select_columns[query.select_column_count], token);
                    query.select_column_count++;
                }
            }
        }
        
        // Final validation: Must have at least a table name
        if (strlen(query.table_name) == 0) {
            strcpy(query.error_msg, "Syntax error: Missing FROM clause or table name.");
            query.is_valid = 0;
        }
    } 
    // =========================================================
    // UNKNOWN / UNSUPPORTED
    // =========================================================
    else {
        strcpy(query.error_msg, "Unsupported or unknown command.");
    }

    free(sql_copy);
    return query;
}