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
    CMD_DROP_DB,         
    CMD_CREATE_TABLE,    
    CMD_DROP_TABLE,      
    CMD_INSERT,          
    CMD_SELECT,
    CMD_DELETE,
    CMD_SHOW_DB,         // NEW: SHOW DATABASES;
    CMD_SHOW_TABLES      // NEW: SHOW TABLES;
} CommandType;

// --- Supporting Structures ---
typedef struct {
    char name[MAX_COLUMN_NAME_LEN];
    char type[MAX_TYPE_LEN];
    int is_primary_key;  
    int is_not_null;     
} ColumnDef;

// --- Main Parse Result ---
typedef struct {
    CommandType type;
    
    char db_name[MAX_DB_NAME_LEN];       
    char table_name[MAX_TABLE_NAME_LEN]; 
    
    ColumnDef columns[MAX_COLUMNS];
    int column_count;
    
    char values[MAX_VALUES][MAX_VALUE_LEN]; 
    int value_count;
    
    char select_columns[MAX_COLUMNS][MAX_COLUMN_NAME_LEN]; 
    int select_column_count; 
    
    int has_where;                             
    char where_column[MAX_COLUMN_NAME_LEN];
    char where_operator[4]; 
    char where_value[MAX_VALUE_LEN];
    
    int has_join;                              
    char join_table[MAX_TABLE_NAME_LEN];
    char join_condition_left[MAX_COLUMN_NAME_LEN];  
    char join_condition_right[MAX_COLUMN_NAME_LEN]; 
    
    int is_valid; 
    char error_msg[MAX_ERROR_MSG_LEN];

    char* bulk_insert_ptr; 

    char order_by_column[64];
    int has_order_by;
    int order_by_desc; // NEW: 0 for ASC, 1 for DESC
} ParsedQuery;

// --- Function Prototypes ---
ParsedQuery parse_sql(const char* sql_string);

#endif