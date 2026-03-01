#define _POSIX_C_SOURCE 200809L
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/time.h>
#include <fcntl.h>
#include "httpParser.h"
#include "handlers.h"
#include "connection.h"

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_REQUEST_LIMIT 16384 // This is what max request can be

int main() {
    int server_fd, new_socket, docroot_fd;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    int opt = 1;
    // struct timeval timeout;
    // timeout.tv_sec = 10;
    // timeout.tv_usec = 0;

    // Open file descriptor to server_file_root folder
    if ((docroot_fd = open("public", O_RDONLY | O_DIRECTORY)) == -1) {
        perror("open failed");
        exit(EXIT_FAILURE);
    }

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Socket creation failed\n");
        exit(EXIT_FAILURE);
    }
    // Set server_fd to be non-blocking
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    // Configure socket to allow port reuse
    if ((setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
        fprintf(stderr, "Socket address setup failed\n");
        exit(EXIT_FAILURE);
    }
    // Set up address structure for binding
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Forcefully binding socket to port 8080
    if (bind(server_fd, (struct sockaddr*)&address, addrlen) < 0) {
        fprintf(stderr, "Socket bind error\n");
        exit(EXIT_FAILURE);
    }

    // Listen to the socket
    if ((listen(server_fd, 50)) < 0) {
        fprintf(stderr, "Error while listening\n");
        exit(EXIT_FAILURE);
    }

    int epoll_fd = epoll_create(1);

    struct epoll_event ev, events[100];
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd");
        exit(EXIT_FAILURE);
    }

    while (1) {
        int socketEvents = epoll_wait(epoll_fd, events, 100, -1);
        if (socketEvents == -1) {
            perror("epoll_wait");
            break;
        }

        for (int i = 0;i < socketEvents;i++) {
            if (events[i].data.fd == server_fd) {
                // Accept new connection
                while ((new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen)) != -1) {
                    // Configure socket to timeout after sometime
                    // if ((setsockopt(new_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) < 0) {
                    //     fprintf(stderr, "Socket timeout setup failed\n");
                    //     exit(EXIT_FAILURE);
                    // }

                    // Set the client socket as non-blocking
                    int flags = fcntl(new_socket, F_GETFL, 0);
                    fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);

                    // Add the new socket to epoll
                    connection_t* conn = malloc(sizeof(connection_t));
                    initializeConnection(conn, new_socket);

                    if (conn->state != READING_HEADERS) {
                        exit(EXIT_FAILURE);
                    }

                    struct epoll_event conn_ev;
                    conn_ev.events = EPOLLIN;
                    conn_ev.data.ptr = conn;

                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &conn_ev) == -1) {
                        perror("epoll_ctl: server_fd");
                        close(new_socket);
                        free(conn);
                    }
                    fprintf(stdout, "Epoll event added for request\n");

                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("accept");
                }
            }
            else {
                connection_t* conn = events[i].data.ptr;
                u_int32_t epoll_events = events[i].events;
                uint32_t mask = connectionHandler(conn, epoll_events);

                if (mask == -1) {
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL) == -1) {
                        perror("epoll_ctl: del epollin");
                        return EXIT_FAILURE;
                    }
                    closeConnection(conn);
                    fprintf(stdout, "Epoll event deleted for request\n");
                }
                else {
                    struct epoll_event temp_ev;
                    temp_ev.data.ptr = conn;
                    temp_ev.events = mask;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &temp_ev) == -1) {
                        perror("epoll_ctl: mod epollin");
                        connectionHandler(conn, EPOLLERR); // This is my invariant
                    }
                    fprintf(stdout, "Epoll event modified for request\n");
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

// fix uint32 using -1