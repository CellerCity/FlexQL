#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "flexql.h" // Ensure this path points to your flexql.h file

// 1. The Opaque Structure
// The user only sees "FlexQL", but internally we know it holds the network socket.
struct FlexQL {
    int sockfd;
};

// 2. Open Connection
int flexql_open(const char *host, int port, FlexQL **db) {
    *db = (FlexQL*)malloc(sizeof(FlexQL));
    if (*db == NULL) return FLEXQL_ERROR;

    (*db)->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if ((*db)->sockfd < 0) {
        free(*db);
        return FLEXQL_ERROR;
    }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        close((*db)->sockfd);
        free(*db);
        return FLEXQL_ERROR;
    }

    if (connect((*db)->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close((*db)->sockfd);
        free(*db);
        return FLEXQL_ERROR;
    }

    // Clear the initial "Welcome" message from the server buffer so it doesn't pollute queries
    char buffer[1024];
    recv((*db)->sockfd, buffer, sizeof(buffer) - 1, 0);

    return FLEXQL_OK;
}

// 3. Close Connection
int flexql_close(FlexQL *db) {
    if (db == NULL) return FLEXQL_ERROR;
    
    // Send an exit command to cleanly shut down the server thread
    send(db->sockfd, ".exit\n", 6, 0); 
    close(db->sockfd);
    free(db);
    
    return FLEXQL_OK;
}

// 4. Execute Query & Stream Results
int flexql_exec(FlexQL *db, const char *sql, int (*callback)(void*, int, char**, char**), void *arg, char **errmsg) {
    if (db == NULL) return FLEXQL_ERROR;
    if (errmsg) *errmsg = NULL;

    // Send the query to the server
    if (send(db->sockfd, sql, strlen(sql), 0) < 0) {
        if (errmsg) *errmsg = strdup("Network error: Failed to send query.");
        return FLEXQL_ERROR;
    }

    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    // Listen for the streaming response
    while (1) {
        int bytes_read = recv(db->sockfd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            if (errmsg) *errmsg = strdup("Network error: Server disconnected.");
            return FLEXQL_ERROR;
        }
        buffer[bytes_read] = '\0';

        // Process line by line (handles cases where multiple rows arrive in one TCP packet)
        char *saveptr_line;
        char *line = strtok_r(buffer, "\n", &saveptr_line);
        
        while (line != NULL) {
            // --- HANDLE SUCCESS ---
            if (strncmp(line, "DONE", 4) == 0) {
                return FLEXQL_OK;
            } 
            // --- HANDLE ERROR ---
            else if (strncmp(line, "ERROR|", 6) == 0) {
                if (errmsg) *errmsg = strdup(line + 6); // Copy error message to pointer
                return FLEXQL_ERROR;
            } 
            // --- HANDLE ROW DATA ---
            else if (strncmp(line, "ROW|", 4) == 0) {
                if (callback != NULL) {
                    char *saveptr_col;
                    strtok_r(line, "|", &saveptr_col); // Skip "ROW"
                    
                    char *col_count_str = strtok_r(NULL, "|", &saveptr_col);
                    if (col_count_str) {
                        int col_count = atoi(col_count_str);
                        
                        // Allocate arrays to hold the pointers to names and values
                        char **colNames = (char**)malloc(col_count * sizeof(char*));
                        char **values = (char**)malloc(col_count * sizeof(char*));
                        
                        for (int i = 0; i < col_count; i++) {
                            colNames[i] = strtok_r(NULL, "|", &saveptr_col);
                            values[i] = strtok_r(NULL, "|", &saveptr_col);
                        }
                        
                        // FIRE THE CALLBACK! [cite: 139]
                        int abort_flag = callback(arg, col_count, values, colNames);
                        
                        free(colNames);
                        free(values);
                        
                        // If callback returns 1, the user wants to abort [cite: 148, 149]
                        if (abort_flag == 1) {
                            return FLEXQL_OK; 
                        }
                    }
                }
            } 
            // --- FALLBACK (Just in case the server sent an old format string) ---
            else {
                if (strstr(line, "[-]")) {
                    if (errmsg) *errmsg = strdup(line);
                    return FLEXQL_ERROR;
                } else if (strstr(line, "[+]")) {
                    return FLEXQL_OK;
                }
            }
            
            line = strtok_r(NULL, "\n", &saveptr_line);
        }
    }
    return FLEXQL_OK;
}

// 5. Free Memory
void flexql_free(void *ptr) {
    if (ptr != NULL) {
        free(ptr); // [cite: 152]
    }
}