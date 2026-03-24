#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include "flexql.h" // Ensure this path points to your flexql.h file

// 1. The Opaque Structure
// The user only sees "FlexQL", but internally we know it holds the network socket.
struct FlexQL {
    int sockfd;
    char write_buffer[65536]; // The 64KB Pipeline Buffer
    int buffer_pos;
};

// 2. Open Connection
int flexql_open(const char *host, int port, FlexQL **db) {
    *db = (FlexQL*)malloc(sizeof(FlexQL));
    if (*db == NULL) return FLEXQL_ERROR;

    (*db)->sockfd = socket(AF_INET, SOCK_STREAM, 0);

    // --- THE FIX: Disable Nagle's Algorithm ---
    int flag = 1;
    setsockopt((*db)->sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    // ------------------------------------------

    if ((*db)->sockfd < 0) {
        free(*db);
        return FLEXQL_ERROR;
    }

    (*db)->buffer_pos = 0;
    memset((*db)->write_buffer, 0, sizeof((*db)->write_buffer));

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
    
    // THE FIX: Flush any remaining buffered queries before shutting down!
    if (db->buffer_pos > 0) {
        send(db->sockfd, db->write_buffer, db->buffer_pos, 0);
        db->buffer_pos = 0;
    }
    
    // Send an exit command to cleanly shut down the server thread
    send(db->sockfd, ".exit\n", 6, 0); 
    close(db->sockfd);
    free(db);
    
    return FLEXQL_OK;
}

// 4. Execute Query & Stream Results
// 4. Execute Query & Stream Results
int flexql_exec(FlexQL *db, const char *sql, int (*callback)(void*, int, char**, char**), void *arg, char **errmsg) {
    if (db == NULL) return FLEXQL_ERROR;
    if (errmsg) *errmsg = NULL;

    // =========================================================
    // THE PIPELINING MAGIC (FIRE AND FORGET)
    // =========================================================
    // Is it an INSERT query? Buffer it!
    if (strncasecmp(sql, "INSERT", 6) == 0) {
        int sql_len = strlen(sql);
        
        // If the buffer is about to overflow, flush it to the server!
        if (db->buffer_pos + sql_len + 2 >= sizeof(db->write_buffer)) {
            send(db->sockfd, db->write_buffer, db->buffer_pos, 0);
            db->buffer_pos = 0; 
            memset(db->write_buffer, 0, sizeof(db->write_buffer));
        }

        // Add the query and a newline to the buffer
        strcpy(db->write_buffer + db->buffer_pos, sql);
        db->buffer_pos += sql_len;
        db->write_buffer[db->buffer_pos] = '\n';
        db->buffer_pos++;

        return FLEXQL_OK; // Fire and forget!
    }

    // =========================================================
    // If it is NOT an insert (e.g., SELECT, CREATE), we MUST 
    // flush the buffer first so the server has all the data!
    // =========================================================
    if (db->buffer_pos > 0) {
        send(db->sockfd, db->write_buffer, db->buffer_pos, 0);
        db->buffer_pos = 0;
        memset(db->write_buffer, 0, sizeof(db->write_buffer));
    }

    // --- NOW we send the actual SELECT/CREATE query ---
    char network_query[2048];
    snprintf(network_query, sizeof(network_query), "%s\n", sql);

    if (send(db->sockfd, network_query, strlen(network_query), 0) < 0) {
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
                        
                        // FIRE THE CALLBACK!
                        int abort_flag = callback(arg, col_count, values, colNames);
                        
                        free(colNames);
                        free(values);
                        
                        // If callback returns 1, the user wants to abort
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