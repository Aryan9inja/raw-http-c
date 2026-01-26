#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define PORT 8080

int main() {
    int server_fd, new_socket;
    int opt = 1;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Setting socket options, specially port
    if ((setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
        perror("Socket option setup failed");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Forcefully binding socket to port 8080
    if (bind(server_fd, (struct sockaddr*)&address, addrlen) < 0) {
        perror("Socket bind error");
        exit(EXIT_FAILURE);
    }

    // Listen to the socket
    if ((listen(server_fd, 3)) < 0) {
        perror("Error while listening");
        exit(EXIT_FAILURE);
    }

    // Accept new connection
    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
        perror("Error while accepting");
        exit(EXIT_FAILURE);
    }

    size_t bufferSize = 1024;
    char* buffer = malloc(sizeof(char) * bufferSize);
    if (buffer == NULL) {
        perror("Buffer Malloc Failed");
        exit(EXIT_FAILURE);
    }

    ssize_t valread;
    size_t readOffset = 0;
    char* headerEnd = NULL;
    while (1) {
        valread = read(new_socket, buffer + readOffset, bufferSize - 1 - readOffset);

        if (valread > 0) {
            readOffset += valread;
            buffer[readOffset] = '\0';
        }
        else if (valread == 0) {
            // Client closed the connection
            break;
        }
        else {
            perror("Read Error");
            break;
        }

        if ((headerEnd = strstr(buffer, "\r\n\r\n")) != NULL) {
            printf("Header end is \n%s\n", headerEnd);
            //Parse header/request here using header end
            break;
        }
    }

    free(buffer);
    close(new_socket);
    close(server_fd);

    return EXIT_SUCCESS;
}