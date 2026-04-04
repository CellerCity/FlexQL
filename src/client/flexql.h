#ifndef FLEXQL_H
#define FLEXQL_H

#ifdef __cplusplus
extern "C" {
#endif

// Required Error Codes
#define FLEXQL_OK 0
#define FLEXQL_ERROR 1

// Opaque structure - the user never sees what is inside this!
typedef struct FlexQL FlexQL;

// Establishes a connection to the FlexQL database server.
int flexql_open(const char *host, int port, FlexQL **db);

// Closes the connection to the FlexQL server and releases resources.
int flexql_close(FlexQL *db);

// Executes an SQL statement on the FlexQL database server.
int flexql_exec(
    FlexQL *db, 
    const char *sql, 
    int (*callback)(void*, int, char**, char**), 
    void *arg, 
    char **errmsg
);

// Frees memory allocated by the FlexQL API (like the errmsg).
void flexql_free(void *ptr);


#ifdef __cplusplus
}
#endif

#endif