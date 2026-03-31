#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 9000
#define BUFFER_SIZE (1024 * 1024)

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server_addr;
    char* buffer = malloc(BUFFER_SIZE);
    
    const char *ip = (argc > 1) ? argv[1] : SERVER_IP;
    int port = (argc > 2) ? atoi(argv[2]) : PORT;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[-] Socket creation failed");
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("[-] Invalid address / Address not supported");
        close(sock);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Connection to server failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    int bytes_read = read(sock, buffer, BUFFER_SIZE - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        if (strncmp(buffer, "CONNECTED", 9) == 0) {
            printf("\n========================================\n");
            printf("  Welcome to the FlexQL Database CLI    \n");
            printf("  Connected to %s:%d\n", ip, port);
            printf("  Type '.exit' to quit.\n");
            printf("========================================\n\n");
        }
    }

    char input[2048];
    while (1) {
        printf("flexql> ");
        fflush(stdout); // Keeps the prompt clean during mass copy-pasting
        
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0; 

        if (strcmp(input, ".exit") == 0) {
            printf("Closing connection. Goodbye!\n");
            break;
        }
        if (strlen(input) == 0) continue; 

        // Send query synchronously
        char network_query[2048];
        snprintf(network_query, sizeof(network_query), "%s\n", input);
        if (send(sock, network_query, strlen(network_query), 0) < 0) break;

        // Wait for the server's true response
        int server_done = 0;
        int rows_returned = 0;

        while (!server_done) {
            memset(buffer, 0, BUFFER_SIZE);
            bytes_read = read(sock, buffer, BUFFER_SIZE - 1);
            if (bytes_read <= 0) {
                printf("\n[-] Server disconnected abruptly.\n");
                server_done = 1; break;
            }
            buffer[bytes_read] = '\0';

            char *saveptr_line;
            char *line = strtok_r(buffer, "\n", &saveptr_line);
            
            while (line != NULL) {
                if (strncmp(line, "DONE|", 5) == 0) {
                    if (rows_returned > 0) printf("(%d rows returned)\n", rows_returned);
                    printf("[+] %s\n\n", line + 5);
                    server_done = 1;
                } else if (strncmp(line, "ERROR|", 6) == 0) {
                    printf("[-] %s\n\n", line + 6);
                    server_done = 1;
                } else if (strncmp(line, "ROW|", 4) == 0) {
                    char *saveptr_col; strtok_r(line, "|", &saveptr_col);
                    char *col_count_str = strtok_r(NULL, "|", &saveptr_col);
                    if (col_count_str) {
                        int cols = atoi(col_count_str);
                        printf("  > ");
                        for (int i = 0; i < cols; i++) {
                            char *name = strtok_r(NULL, "|", &saveptr_col);
                            char *val = strtok_r(NULL, "|", &saveptr_col);
                            if (name && val) {
                                printf("%s: %s", name, val);
                                if (i < cols - 1) printf("  |  ");
                            }
                        }
                        printf("\n");
                        rows_returned++;
                    }
                } else {
                    // Safety net for unexpected strings
                    if (strlen(line) > 0 && strcmp(line, "\r") != 0) printf("%s\n", line);
                }
                line = strtok_r(NULL, "\n", &saveptr_line);
            }
        }
    }
    free(buffer); close(sock);
    return 0;
}