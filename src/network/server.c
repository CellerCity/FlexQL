#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/stat.h>

#include "../parser/parser.h"
#include "../storage/executor.h"
#include "../storage/pager.h"

#define PORT 9000

// --- The Multithreaded Client Handler ---
void* client_handler(void* socket_desc) {
    int client_sock = *(int*)socket_desc;
    free(socket_desc); 

    // --- THE DEFAULT DB FIX ---
    // Auto-assign the client to a default database and ensure the folder exists!
    char current_db[MAX_DB_NAME_LEN] = "default_db"; 
    mkdir(current_db, 0777); 

    uint32_t active_root_page = 0; 
    uint32_t active_data_page = 1; 
    Pager* active_pager = NULL;
    char active_table[256] = "";

    char* welcome = "CONNECTED\n";
    send(client_sock, welcome, strlen(welcome), 0);

    // --- THE BUFFER OVERFLOW FIX ---
    // Use the heap (malloc) so we can hold the TA's massive 250KB string without crashing!
    size_t MAX_BUFFER = 10 * 1024 * 1024; // 10 Megabytes
    char* stream_buffer = malloc(MAX_BUFFER);
    stream_buffer[0] = '\0';
    char* read_buffer = malloc(65536);
    char* client_message = malloc(MAX_BUFFER);

    int bytes_read;
    // Safely read up to 65535 bytes at a time
    while ((bytes_read = recv(client_sock, read_buffer, 65535, 0)) > 0) {
        read_buffer[bytes_read] = '\0'; // Safety null termination exactly at the bytes read
        
        // Append newly received raw bytes to the master stream buffer
        strcat(stream_buffer, read_buffer);

        char *newline_ptr;
        while ((newline_ptr = strchr(stream_buffer, '\n')) != NULL) {
            
            *newline_ptr = '\0'; // Split the string at the newline
            strcpy(client_message, stream_buffer); // Extract the single query

            // Shift the rest of the stream buffer to the left
            memmove(stream_buffer, newline_ptr + 1, strlen(newline_ptr + 1) + 1);

            client_message[strcspn(client_message, "\r")] = 0;
            if (strlen(client_message) == 0) continue;
            if (strcmp(client_message, ".exit") == 0) goto graceful_shutdown;

            ParsedQuery q = parse_sql(client_message);

            if (!q.is_valid) {
                char error_response[512];
                snprintf(error_response, sizeof(error_response), "ERROR|Syntax Error: %s\n", q.error_msg);
                send(client_sock, error_response, strlen(error_response), 0);
                continue; // Skip processing
            }

            char response[512] = "DONE|Query executed successfully.\n";
            int send_default_response = 1;        

            if (q.type == CMD_CREATE_DB) {
                if (execute_create_db(&q) != 0) strcpy(response, "ERROR|Failed to create database.\n");
            } else if (q.type == CMD_USE_DB) {
                if (execute_use_db(&q, current_db) != 0) strcpy(response, "ERROR|Database does not exist.\n");
            } else if (q.type == CMD_DROP_DB) {
                execute_drop_db(&q, current_db);
            } else if (q.type == CMD_CREATE_TABLE) {
                execute_create(current_db, &q);
            } else if (q.type == CMD_DROP_TABLE) {
                execute_drop_table(current_db, &q);
            } else if (q.type == CMD_DELETE) {
                execute_delete(current_db, &q);
                if (active_pager != NULL && strcmp(active_table, q.table_name) == 0) {
                    pager_close(active_pager);
                    active_pager = NULL;
                    active_table[0] = '\0';
                }
            } else if (q.type == CMD_INSERT || q.type == CMD_SELECT) {
                
                if (active_pager == NULL || strcmp(active_table, q.table_name) != 0) {
                    if (active_pager != NULL) pager_close(active_pager); 
                    char filepath[512];
                    snprintf(filepath, sizeof(filepath), "%s/%s.dat", current_db, q.table_name);
                    active_pager = pager_open(filepath);
                    strcpy(active_table, q.table_name);
                }
                
                if (q.type == CMD_INSERT) {
                    execute_insert(current_db, active_pager, &active_root_page, &active_data_page, &q);
                    send_default_response = 0;
                } else if (q.type == CMD_SELECT) {
                    send_default_response = 0; 
                    execute_select(current_db, active_pager, active_root_page, &q, client_sock);
                }
            } else {
                strcpy(response, "ERROR|Unrecognized command type.\n");
            }

            if (send_default_response) {
                send(client_sock, response, strlen(response), 0);
            }
        }
    }

graceful_shutdown:
    if (active_pager != NULL) pager_close(active_pager); 
    
    // Clean up our massive heap allocations!
    free(stream_buffer);
    free(read_buffer);
    free(client_message);
    
    printf("[Server] Client disconnected.\n");
    close(client_sock);
    return NULL;
}


int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    // 1. Create the socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        perror("[-] Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Allow port reuse to prevent "Address already in use" errors during testing
    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Configure the server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    server_addr.sin_port = htons(PORT);

    // 3. Bind the socket to the port
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    // 4. Listen for incoming connections
    if (listen(server_socket, 10) < 0) {
        perror("[-] Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("[*] FlexQL Server listening on port %d...\n", PORT);

    // 5. Main Loop: Accept clients continuously
    while (1) {
        client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &addr_len);
        if (client_socket < 0) {
            perror("[-] Accept failed");
            continue;
        }

        // --- THE FIX: Disable Nagle's Algorithm for the server responses ---
        int flag = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
        // -------------------------------------------------------------------

        printf("[+] New connection accepted from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Allocate memory for the client socket to pass to the thread safely
        // This prevents race conditions where the main thread overwrites client_socket
        int *new_sock = malloc(sizeof(int));
        *new_sock = client_socket;

        // Create a new thread for the client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, client_handler, (void *)new_sock) < 0) {
            perror("[-] Could not create thread");
            free(new_sock);
            close(client_socket);
            continue;
        }

        // Detach the thread so its resources are automatically freed upon completion
        pthread_detach(thread_id);
    }

    close(server_socket);
    return 0;
}



// gcc src/network/server.c src/storage/executor.c src/storage/schema.c src/storage/btree.c src/storage/pager.c src/parser/parser.c -o flexql-server -lpthread