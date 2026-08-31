#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include "flexql.h" 

struct FlexQL {
    int sockfd;
};

int flexql_open(const char *host, int port, FlexQL **db) {
    *db = (FlexQL*)malloc(sizeof(FlexQL));
    if (*db == NULL) return FLEXQL_ERROR;

    (*db)->sockfd = socket(AF_INET, SOCK_STREAM, 0); // Creates an IPv4 (AF_INET), two-way, connection-based byte stream (SOCK_STREAM implies TCP) socket. It stores the resulting file descriptor in the sockfd member of the struct.

    int flag = 1;
    setsockopt((*db)->sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int)); // TCP_NODELAY: This is a crucial setting for database drivers. By default, TCP uses Nagle's Algorithm, which buffers small packets to send them in one larger chunk to save bandwidth. Setting TCP_NODELAY to 1 disables Nagle's algorithm, forcing the socket to send packets immediately. This heavily reduces latency for small database queries.

    if ((*db)->sockfd < 0) { free(*db); return FLEXQL_ERROR; }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    // This prepares the sockaddr_in structure with the server's details. AF_INET specifies IPv4. The htons(port) function converts the port number from host byte order to network byte order (big-endian), which is required by network protocols.


    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0 ||
        connect((*db)->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close((*db)->sockfd); free(*db); return FLEXQL_ERROR;
    }

    // inet_pton: Converts the IP address from a text string (like "192.168.1.5") into a binary network format. If the IP is invalid, it fails.
    // connect: Attempts to establish a TCP handshake with the remote server using the configured address and port.
    // Cleanup: If either the IP translation or the connection fails, it performs a clean exit by closing the socket, freeing the struct memory, and returning an error.

    char buffer[1024];
    recv((*db)->sockfd, buffer, sizeof(buffer) - 1, 0); // Clear welcome message
    return FLEXQL_OK;
}

int flexql_close(FlexQL *db) {
    if (db == NULL) return FLEXQL_ERROR;
    send(db->sockfd, ".exit\n", 6, 0); 
    close(db->sockfd);
    free(db);
    return FLEXQL_OK;
}

int flexql_exec(FlexQL *db, const char *sql, int (*callback)(void*, int, char**, char**), void *arg, char **errmsg) {
    if (db == NULL) return FLEXQL_ERROR;
    if (errmsg) *errmsg = NULL;

    // --- 1. SEND QUERY ---
    int sql_len = strlen(sql);
    char* network_query = (char*)malloc(sql_len + 2);
    sprintf(network_query, "%s\n", sql);
    
    //The server protocol clearly expects commands to be terminated by a newline (\n).
    // The function allocates a buffer large enough for the SQL string plus a newline and a null terminator.
    // It appends the newline using sprintf.
    // It sends the query over the socket using send(). If send fails (e.g., the network drops), it populates the errmsg and returns FLEXQL_ERROR.

    if (send(db->sockfd, network_query, sql_len + 1, 0) < 0) {
        free(network_query);
        if (errmsg) *errmsg = strdup("Network error: Failed to send query.");
        return FLEXQL_ERROR;
    }
    free(network_query);

    // --- 2. SYNCHRONOUSLY WAIT FOR TRUE RESPONSE ---
    char buffer[4096];
    while (1) {
        // the client cannot load everything into memory at once. It enters an infinite loop, reading data in 4KB chunks (4096 bytes).
        int bytes_read = recv(db->sockfd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            if (errmsg) *errmsg = strdup("Network error: Server disconnected.");
            return FLEXQL_ERROR;
        }
        buffer[bytes_read] = '\0';

        char *saveptr_line;
        char *line = strtok_r(buffer, "\n", &saveptr_line);
        // Reentrant: Safe for multi-threaded applications because it does not use internal static storage.

        
        while (line != NULL) {
            if (strncmp(line, "DONE", 4) == 0) return FLEXQL_OK;
            else if (strncmp(line, "ERROR|", 6) == 0) {
                if (errmsg) *errmsg = strdup(line + 6); 
                return FLEXQL_ERROR;
            } 
            else if (strncmp(line, "ROW|", 4) == 0) {
                // ROW|<col_count>|<col1_name>|<col1_value>|<col2_name>|<col2_value>...
                if (callback != NULL) {
                    char *saveptr_col; strtok_r(line, "|", &saveptr_col); 
                    char *col_count_str = strtok_r(NULL, "|", &saveptr_col);
                    if (col_count_str) {
                        int col_count = atoi(col_count_str);
                        char **colNames = (char**)malloc(col_count * sizeof(char*));
                        char **values = (char**)malloc(col_count * sizeof(char*));
                        
                        // Extract the columns from the TCP packet (ONLY ONCE!)
                        for (int i = 0; i < col_count; i++) {
                            colNames[i] = strtok_r(NULL, "|", &saveptr_col);
                            values[i] = strtok_r(NULL, "|", &saveptr_col);
                        }
                        
                        // Execute the callback
                        int abort_flag = callback(arg, col_count, values, colNames);
                        
                        free(colNames); 
                        free(values);
                        
                        if (abort_flag == 1) return FLEXQL_OK;
                    }
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
        free(ptr);
    }
}