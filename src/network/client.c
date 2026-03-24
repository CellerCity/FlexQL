#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERVER_IP "127.0.0.1"
#define PORT 9000
#define BUFFER_SIZE 2048

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char server_response[BUFFER_SIZE];

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

    printf("[+] Connected to FlexQL server at %s:%d\n\n", ip, port);

    // --- FIX 1: Catch the initial welcome message! ---
    int bytes_read = read(sock, server_response, sizeof(server_response) - 1);
    if (bytes_read > 0) {
        server_response[bytes_read] = '\0';
        printf("%s", server_response); // Print the server's welcome and "> " prompt
        fflush(stdout); // Ensure it prints immediately
    }

    // 4. Main REPL Loop
    while (1) {
        // FIX 2: Removed "flexql> " because the server provides the "> " prompt!
        
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) break;

        buffer[strcspn(buffer, "\n")] = 0;

        if (strcmp(buffer, ".exit") == 0) {
            printf("Connection closed\n");
            break;
        }

        if (strlen(buffer) == 0) {
            // If user hits enter, send a blank space to trigger the server's empty-check
            send(sock, " ", 1, 0); 
        } else {
            if (send(sock, buffer, strlen(buffer), 0) < 0) break;
        }

        // Wait for the server's response
        bytes_read = read(sock, server_response, sizeof(server_response) - 1);
        if (bytes_read > 0) {
            server_response[bytes_read] = '\0';
            printf("%s", server_response); // Prints the response and the next "> "
            fflush(stdout);
        } else if (bytes_read == 0) {
            printf("\n[-] Server closed the connection.\n");
            break;
        } else {
            perror("[-] Read failed");
            break;
        }
    }

    close(sock);
    return 0;
}



// gcc src/network/client.c -o flexql-client