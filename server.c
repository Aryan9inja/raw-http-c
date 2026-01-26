#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "httpParser.h"

#define PORT 8080

int main() {
    int server_fd, new_socket;
    ssize_t valread;
    int opt = 1;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    char buffer[1024] = { 0 };

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

    int total_bytes = 0;
    while (1) {
        valread = read(new_socket, buffer + total_bytes, 1024 - total_bytes - 1);
        if (valread <= 0) break;

        total_bytes += valread;
        char* buffPtr = NULL;

        if ((buffPtr = strstr(buffer, "\r\n\r\n")) != NULL) {
            httpInfo_t httpInfo = extractHttpInfo(buffer, &buffPtr);

            break;
        }

        if (total_bytes >= 1023) {
            printf("Buffer end before http parsing");
            break;
        }
    }

    close(new_socket);
    close(server_fd);

    return EXIT_SUCCESS;
}