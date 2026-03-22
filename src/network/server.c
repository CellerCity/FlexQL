#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 9000
#define BUFFER_SIZE 1024

// This function is executed by each new worker thread
void *handle_client(void *client_socket_ptr) {
    int client_socket = *((int *)client_socket_ptr);
    free(client_socket_ptr); // Free the allocated memory for the socket pointer
    
    char buffer[BUFFER_SIZE];
    int bytes_read;

    printf("[+] Thread %lu handling new client.\n", pthread_self());

    // Continuously listen to this specific client
    while ((bytes_read = read(client_socket, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0'; // Null-terminate the received string
        
        // Strip newline for cleaner printing
        buffer[strcspn(buffer, "\n")] = 0; 

        printf("[Client] %s\n", buffer);

        // Prepare and send the ACK back to the client
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), "ACK: Received '%s'", buffer);
        write(client_socket, response, strlen(response));
    }

    if (bytes_read == 0) {
        printf("[-] Client disconnected.\n");
    } else {
        perror("[-] Client read error");
    }

    close(client_socket);
    pthread_exit(NULL); // Terminate this thread
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

        printf("[+] New connection accepted from %s:%d\n", 
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Allocate memory for the client socket to pass to the thread safely
        // This prevents race conditions where the main thread overwrites client_socket
        int *new_sock = malloc(sizeof(int));
        *new_sock = client_socket;

        // Create a new thread for the client
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)new_sock) < 0) {
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