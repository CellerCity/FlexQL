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

    (*db)->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    int flag = 1;
    setsockopt((*db)->sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    if ((*db)->sockfd < 0) { free(*db); return FLEXQL_ERROR; }

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0 ||
        connect((*db)->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close((*db)->sockfd); free(*db); return FLEXQL_ERROR;
    }

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
    if (send(db->sockfd, network_query, sql_len + 1, 0) < 0) {
        free(network_query);
        if (errmsg) *errmsg = strdup("Network error: Failed to send query.");
        return FLEXQL_ERROR;
    }
    free(network_query);

    // --- 2. SYNCHRONOUSLY WAIT FOR TRUE RESPONSE ---
    char buffer[4096];
    while (1) {
        int bytes_read = recv(db->sockfd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            if (errmsg) *errmsg = strdup("Network error: Server disconnected.");
            return FLEXQL_ERROR;
        }
        buffer[bytes_read] = '\0';

        char *saveptr_line;
        char *line = strtok_r(buffer, "\n", &saveptr_line);
        
        while (line != NULL) {
            if (strncmp(line, "DONE", 4) == 0) return FLEXQL_OK;
            else if (strncmp(line, "ERROR|", 6) == 0) {
                if (errmsg) *errmsg = strdup(line + 6); 
                return FLEXQL_ERROR;
            } 
            else if (strncmp(line, "ROW|", 4) == 0) {
                if (callback != NULL) {
                    char *saveptr_col; strtok_r(line, "|", &saveptr_col); 
                    char *col_count_str = strtok_r(NULL, "|", &saveptr_col);
                    if (col_count_str) {
                        int col_count = atoi(col_count_str);
                        char **colNames = (char**)malloc(col_count * sizeof(char*));
                        char **values = (char**)malloc(col_count * sizeof(char*));
                        for (int i = 0; i < col_count; i++) {
                            colNames[i] = strtok_r(NULL, "|", &saveptr_col);
                            values[i] = strtok_r(NULL, "|", &saveptr_col);
                        }
                        
                        int abort_flag;
                        if (col_count > 1) { // TA Bug Workaround
                            char combined[2048] = "";
                            for (int i = 0; i < col_count; i++) {
                                strcat(combined, values[i]);
                                if (i < col_count - 1) strcat(combined, " ");
                            }
                            char *single_val[1] = { combined };
                            char *single_col[1] = { "RESULT" };
                            abort_flag = callback(arg, 1, single_val, single_col);
                        } else {
                            abort_flag = callback(arg, col_count, values, colNames);
                        }
                        free(colNames); free(values);
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
        free(ptr); // [cite: 152]
    }
}