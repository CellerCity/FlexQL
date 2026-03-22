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
    memset(&query, 0, sizeof(ParsedQuery)); // Initialize struct with zeros
    query.type = CMD_UNKNOWN;
    query.is_valid = 0;

    // Make a working copy of the string (strtok modifies the original)
    char sql_copy[1024];
    strncpy(sql_copy, sql_string, sizeof(sql_copy) - 1);
    sql_copy[sizeof(sql_copy) - 1] = '\0';

    // Extract the first keyword
    char *token = strtok(sql_copy, " \t\n");
    if (!token) {
        strcpy(query.error_msg, "Empty query.");
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
    else if (token && strcasecmp(token, "TABLE") == 0) {
            query.type = CMD_CREATE_TABLE;
            token = strtok(NULL, " \t\n(");
            if (token) {
                strcpy(query.table_name, token);
                
                const char *start_paren = strchr(sql_string, '(');
                const char *end_paren = strrchr(sql_string, ')');
                
                if (start_paren && end_paren && start_paren < end_paren) {
                    char cols_str[512];
                    int len = end_paren - start_paren - 1;
                    strncpy(cols_str, start_paren + 1, len);
                    cols_str[len] = '\0';
                    
                    int pk_count = 0; // Track Primary Keys
                    query.is_valid = 1; // Assume valid until proven otherwise
                    
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
                                // --- TYPE VALIDATION ---
                                if (strcasecmp(word, "INT") != 0 &&
                                    strcasecmp(word, "DECIMAL") != 0 &&
                                    strcasecmp(word, "VARCHAR") != 0 &&
                                    strcasecmp(word, "TEXT") != 0 && 
                                    strcasecmp(word, "DATETIME") != 0) {
                                    
                                    snprintf(query.error_msg, sizeof(query.error_msg), 
                                             "Syntax error: Invalid type '%s'. Must be INT, DECIMAL, VARCHAR, TEXT, or DATETIME.", word);
                                    query.is_valid = 0;
                                    break;
                                }
                                strcpy(query.columns[query.column_count].type, word); 
                                
                                // --- CONSTRAINT EXTRACTION ---
                                while ((word = strtok(NULL, " \t")) != NULL) {
                                    if (strcasecmp(word, "PRIMARY") == 0) {
                                        query.columns[query.column_count].is_primary_key = 1;
                                        pk_count++;
                                    } else if (strcasecmp(word, "NOT") == 0) {
                                        query.columns[query.column_count].is_not_null = 1;
                                    }
                                }
                                
                                // --- MULTIPLE PK VALIDATION ---
                                if (pk_count > 1) {
                                    strcpy(query.error_msg, "Syntax error: A table can only have one PRIMARY KEY.");
                                    query.is_valid = 0;
                                    break;
                                }
                                
                                query.column_count++;
                            } else {
                                strcpy(query.error_msg, "Syntax error: Missing data type for column.");
                                query.is_valid = 0;
                                break;
                            }
                        }
                        col_token = strtok(NULL, ",");
                    }
                } else {
                    strcpy(query.error_msg, "Syntax error: Missing parentheses in CREATE TABLE.");
                    query.is_valid = 0;
                }
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
    // Syntax: INSERT INTO table_name VALUES (...);
    // =========================================================
    else if (strcasecmp(token, "INSERT") == 0) {
        token = strtok(NULL, " \t\n");
        if (token && strcasecmp(token, "INTO") == 0) {
            token = strtok(NULL, " \t\n");
            if (token) {
                strcpy(query.table_name, token);
                
                token = strtok(NULL, " \t\n(");
                if (token && strcasecmp(token, "VALUES") == 0) {
                    
                    const char *start_paren = strchr(sql_string, '(');
                    const char *end_paren = strrchr(sql_string, ')');
                    
                    if (start_paren && end_paren && start_paren < end_paren) {
                        char vals_str[512];
                        int len = end_paren - start_paren - 1;
                        strncpy(vals_str, start_paren + 1, len);
                        vals_str[len] = '\0';
                        
                        // Parse values separated by commas
                        char *val_token = strtok(vals_str, ",");
                        while (val_token && query.value_count < MAX_VALUES) {
                            trim_string(val_token);
                            strcpy(query.values[query.value_count], val_token);
                            query.value_count++;
                            val_token = strtok(NULL, ",");
                        }
                        query.is_valid = 1;
                    } else {
                        strcpy(query.error_msg, "Syntax error: Missing parentheses in VALUES clause.");
                    }
                } else {
                     strcpy(query.error_msg, "Syntax error: Expected VALUES keyword.");
                }
            }
        } else {
             strcpy(query.error_msg, "Syntax error: Expected INTO after INSERT.");
        }
    }
    // =========================================================
    // 5. SELECT COMMAND
    // Syntax: SELECT col1, col2 FROM tableA [INNER JOIN tableB ON A.c1 = B.c2] [WHERE col = val];
    // =========================================================
    else if (strcasecmp(token, "SELECT") == 0) {
        query.type = CMD_SELECT;
        query.is_valid = 1; 
        
        // State tracking: 1=Columns, 2=From, 3=Join, 4=Where
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
                
                token = strtok(NULL, " \t\n;"); // Operator (=, >, <)
                if (token) strcpy(query.where_operator, token);
                
                token = strtok(NULL, " \t\n;"); // Value
                if (token) {
                    trim_string(token); // Remove quotes if it's a string like 'Alice'
                    strcpy(query.where_value, token);
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

    return query;
}