#ifndef PARSER_H
#define PARSER_H

// --- System Limits ---
#define MAX_DB_NAME_LEN 64
#define MAX_TABLE_NAME_LEN 64
#define MAX_COLUMN_NAME_LEN 64
#define MAX_TYPE_LEN 32
#define MAX_COLUMNS 20         
#define MAX_VALUES 20          
#define MAX_VALUE_LEN 128      
#define MAX_ERROR_MSG_LEN 256

// --- SQL Command Types ---
typedef enum {
    CMD_UNKNOWN,
    CMD_CREATE_DB,       
    CMD_USE_DB,          
    CMD_DROP_DB,         // Added: DROP DATABASE db_name;
    CMD_CREATE_TABLE,    
    CMD_DROP_TABLE,      // Added: DROP TABLE table_name;
    CMD_INSERT,          
    CMD_SELECT           
} CommandType;

// --- Supporting Structures ---
typedef struct {
    char name[MAX_COLUMN_NAME_LEN];
    char type[MAX_TYPE_LEN];
    int is_primary_key;  // 1 if PRIMARY KEY, 0 otherwise
    int is_not_null;     // 1 if NOT NULL, 0 otherwise
} ColumnDef;

// --- Main Parse Result ---
typedef struct {
    CommandType type;
    
    // Core Target Identifiers
    char db_name[MAX_DB_NAME_LEN];       
    char table_name[MAX_TABLE_NAME_LEN]; 
    
    // For CREATE TABLE:
    ColumnDef columns[MAX_COLUMNS];
    int column_count;
    
    // For INSERT:
    char values[MAX_VALUES][MAX_VALUE_LEN]; 
    int value_count;
    
    // For SELECT:
    char select_columns[MAX_COLUMNS][MAX_COLUMN_NAME_LEN]; 
    int select_column_count; 
    
    // For WHERE Clause
    int has_where;                             
    char where_column[MAX_COLUMN_NAME_LEN];
    char where_operator[4]; // e.g., "=", ">", "<="
    char where_value[MAX_VALUE_LEN];
    
    // For INNER JOIN
    int has_join;                              
    char join_table[MAX_TABLE_NAME_LEN];
    char join_condition_left[MAX_COLUMN_NAME_LEN];  
    char join_condition_right[MAX_COLUMN_NAME_LEN]; 
    
    // Status tracking
    int is_valid; 
    char error_msg[MAX_ERROR_MSG_LEN];

    char* bulk_insert_ptr; // Holds the massive string of tuples
} ParsedQuery;

// --- Function Prototypes ---
ParsedQuery parse_sql(const char* sql_string);

#endif