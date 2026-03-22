#include <stdio.h>
#include <string.h>
#include "parser.h"

// Helper function to print the parsed struct nicely
void evaluate_test(const char* description, const char* sql) {
    printf("\n==================================================\n");
    printf("TEST: %s\n", description);
    printf("SQL:  %s\n", sql);
    printf("--------------------------------------------------\n");

    ParsedQuery q = parse_sql(sql);

    if (!q.is_valid) {
        printf("[FAILED] Error: %s\n", q.error_msg);
        return;
    }

    printf("[SUCCESS] Command Type: %d\n", q.type);

    if (q.type == CMD_CREATE_DB || q.type == CMD_USE_DB || q.type == CMD_DROP_DB) {
        printf("Database: %s\n", q.db_name);
    }
    
    if (q.type == CMD_CREATE_TABLE) {
        printf("Table: %s\n", q.table_name);
        for (int i = 0; i < q.column_count; i++) {
            printf("  - Col: '%s' | Type: '%s' | PK: %d | NN: %d\n", 
                   q.columns[i].name, q.columns[i].type, 
                   q.columns[i].is_primary_key, q.columns[i].is_not_null);
        }
    }
    
    if (q.type == CMD_INSERT) {
        printf("Table: %s\n", q.table_name);
        for (int i = 0; i < q.value_count; i++) {
            printf("  - Val: '%s'\n", q.values[i]);
        }
    }
    
    if (q.type == CMD_SELECT) {
        if (q.select_column_count == 0) {
            printf("Columns: * (All)\n");
        } else {
            printf("Columns: ");
            for (int i = 0; i < q.select_column_count; i++) {
                printf("'%s' ", q.select_columns[i]);
            }
            printf("\n");
        }
        printf("From Table: %s\n", q.table_name);
        
        if (q.has_join) {
            printf("JOIN: Table '%s' ON %s = %s\n", 
                   q.join_table, q.join_condition_left, q.join_condition_right);
        }
        if (q.has_where) {
            printf("WHERE: '%s' %s '%s'\n", 
                   q.where_column, q.where_operator, q.where_value);
        }
    }
}

int main() {
    // 1. DDL Basics
    evaluate_test("Create DB (Basic)", "CREATE DATABASE testdb;");
    evaluate_test("Drop Table (Messy Spaces)", "   DROP   TABLE    students  ;  ");

    // 2. Create Table Variations
    evaluate_test("Create Table (All 4 Types)", 
                  "CREATE TABLE users (id INT, price DECIMAL, name VARCHAR, created DATETIME);");
    
    evaluate_test("Create Table (PK and NN)", 
                  "CREATE TABLE users (id INT PRIMARY KEY NOT NULL, name VARCHAR);");
    
    evaluate_test("Create Table (Mixed Case & Spacing)", 
                  "cReaTe TaBle   WEIRD_table (  col1   iNt   pRiMaRy    kEy  , col2 varchar NOT null );");
    
    // 3. Insert Variations
    evaluate_test("Insert (Basic)", "INSERT INTO users VALUES (1, 99.99, 'Alice', '2023-10-01');");

    // 4. Select Variations
    evaluate_test("Select (*)", "SELECT * FROM users;");
    
    evaluate_test("Select (Specific Columns)", "SELECT id, name FROM users;");
    
    evaluate_test("Select (Where Clause)", "SELECT * FROM users WHERE age > 21;");
    
    evaluate_test("Select (Inner Join Only)", 
                  "SELECT id, name FROM users INNER JOIN orders ON users.id = orders.user_id;");
    
    evaluate_test("Select (Inner Join AND Where)", 
                  "SELECT * FROM users INNER JOIN orders ON users.id = orders.user_id WHERE status = 'active';");
    
    // Testing the order independence we built!
    evaluate_test("Select (Where AND Inner Join - Swapped Order)", 
                  "SELECT * FROM users WHERE status = 'active' INNER JOIN orders ON users.id = orders.user_id;");


    // Some tough cases:- 

    // 1. The "Happy Path" (From Assignment Doc)
    evaluate_test("Valid Table with PK and NOT NULL", 
                  "CREATE TABLE STUDENT (ID INT PRIMARY KEY NOT NULL, FIRST_NAME TEXT NOT NULL);");

    // 2. Invalid Data Type Test
    evaluate_test("Invalid Data Type (BANANA)", 
                  "CREATE TABLE test (id INT, name BANANA);");

    // 3. Multiple Primary Keys Test
    evaluate_test("Multiple Primary Keys (Should Fail)", 
                  "CREATE TABLE test (id INT PRIMARY KEY, email VARCHAR PRIMARY KEY);");

    // 4. Table-Level Constraint Test
    evaluate_test("Table-level Primary Key (Should Fail)", 
                  "CREATE TABLE test (id INT, name VARCHAR, PRIMARY KEY(id));");


    return 0;
}

// TEST it from the root-dir by:-
// gcc src/parser/test_parser.c src/parser/parser.c -o test_parser
// ./test_parser