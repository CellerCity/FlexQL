#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../client/flexql.h"

// --- The REPL Callback ---
// When the server sends back rows from a SELECT query, flexql_exec() will 
// trigger this function to print them beautifully to the terminal.
int repl_callback(void *data, int columnCount, char **values, char **columnNames) {
    for (int i = 0; i < columnCount; i++) {
        // Print in the format requested by the assignment: COLUMN_NAME = value
        printf("%s = %s\n", columnNames[i] ? columnNames[i] : "UNKNOWN", values[i] ? values[i] : "NULL");
    }
    printf("\n");
    return 0; // Return 0 to continue processing rows
}

int main(int argc, char const *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <IP> <PORT>\n", argv[0]);
        return -1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);

    FlexQL *db = NULL;
    char *errMsg = NULL;

    // 1. Establish the connection using the Driver API
    if (flexql_open(ip, port, &db) != FLEXQL_OK) {
        printf("Connection failed\n");
        return -1;
    }

    printf("Connected to FlexQL server\n\n");

    char input[2048];
    while (1) {
        printf("flexql> ");
        if (fgets(input, sizeof(input), stdin) == NULL) break;

        // Strip the trailing newline character
        input[strcspn(input, "\n")] = 0; 

        // Handle exit commands
        if (strcmp(input, ".exit") == 0 || strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            break;
        }
        
        if (strlen(input) == 0) continue;

        // 2. Execute the query using the Driver API!
        int rc = flexql_exec(db, input, repl_callback, NULL, &errMsg);

        if (rc != FLEXQL_OK) {
            printf("Error: %s\n\n", errMsg ? errMsg : "Unknown execution error");
            if (errMsg) flexql_free(errMsg);
        } else {
            printf("Query executed successfully.\n\n");
        }
    }

    // 3. Gracefully close the connection using the Driver API
    flexql_close(db);
    printf("Connection closed\n");

    return 0;
}