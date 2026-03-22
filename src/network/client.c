#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 9000
#define BUFFER_SIZE 1024

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char server_response[BUFFER_SIZE];

    // Optional: Allow passing IP and Port via command line as per assignment specs
    const char *ip = (argc > 1) ? argv[1] : SERVER_IP;
    int port = (argc > 2) ? atoi(argv[2]) : PORT;

    // 1. Create the socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[-] Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 2. Configure the server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("[-] Invalid address / Address not supported");
        close(sock);
        exit(EXIT_FAILURE);
    }

    // 3. Connect to the server
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("[-] Connection to server failed");
        close(sock);
        exit(EXIT_FAILURE);
    }

    printf("Connected to FlexQL server at %s:%d\n", ip, port);

    // 4. Main REPL Loop
    while (1) {
        printf("flexql> ");
        
        // Get input from the user
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break; // Handle EOF (Ctrl+D)
        }

        // Strip the newline character
        buffer[strcspn(buffer, "\n")] = 0;

        // Check if user wants to exit
        if (strcmp(buffer, ".exit") == 0) {
            printf("Connection closed\n");
            break;
        }

        // Don't send empty strings
        if (strlen(buffer) == 0) {
            continue;
        }

        // Send the command to the server
        if (send(sock, buffer, strlen(buffer), 0) < 0) {
            perror("[-] Send failed");
            break;
        }

        // Wait for the server's response
        int bytes_read = read(sock, server_response, sizeof(server_response) - 1);
        if (bytes_read > 0) {
            server_response[bytes_read] = '\0';
            printf("%s\n", server_response);
        } else if (bytes_read == 0) {
            printf("[-] Server closed the connection.\n");
            break;
        } else {
            perror("[-] Read failed");
            break;
        }
    }

    // 5. Cleanup
    close(sock);
    return 0;
}