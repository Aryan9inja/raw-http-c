#include "connection.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/errno.h>
#include <string.h>
#include <sys/socket.h>
#include "httpParser.h"

#define READ_BUFFER_SIZE 4096 // 4kb
#define MAX_HEADER_SIZE 8192 // 8kb

void initializeConnection(connection_t* conn, int fd) {
    conn->fd = fd;
    conn->state = READING_HEADERS;
    conn->header_end = 0;
    conn->parse_offset = 0;
    conn->read_len = 0;
    conn->read_cap = READ_BUFFER_SIZE;
    conn->read_buf = malloc(READ_BUFFER_SIZE);
    if (!conn->read_buf) {
        perror("Malloc failed");
        conn->state = CLOSING;
        return;
    }
    conn->write_buf = NULL; // so that freeing will not stop the program
    fprintf(stdout, "Connection Initialized\n");
}

void closeConnection(connection_t* conn) {
    close(conn->fd);
    free(conn->read_buf);
    free(conn->write_buf);
    free(conn);
}

void handleHeaders(connection_t* conn) {
    // Check for CRLF to get to header end
    char* header_end = NULL;
    for (size_t i = conn->parse_offset; i + 3 < conn->read_len; i++) {
        if (conn->read_buf[i] == '\r' &&
            conn->read_buf[i + 1] == '\n' &&
            conn->read_buf[i + 2] == '\r' &&
            conn->read_buf[i + 3] == '\n') {
            header_end = conn->read_buf + conn->parse_offset + i;
        }
    }

    if (!header_end) {
        // Wait for next EPOLLIN
        fprintf(stdout, "No header end, returning\n");
        return;
    }
    // Calculate offset so that if later we realloc nothing changes
    conn->header_end = header_end - conn->read_buf;

    size_t header_size = conn->header_end - conn->parse_offset;
    if (header_size > MAX_HEADER_SIZE) {
        handleParseError(MAX_HEADER_SIZE, conn);
        if (conn->state == CLOSING) {
            return;
        }
        return;
    }

    // parserResult_t requestAndHeaderParser()
    fprintf(stdout, "Calling parser\n");
    parserResult_t headerParsingRes = requestAndHeaderParser(
        conn->read_buf + conn->parse_offset,
        conn->read_buf + conn->header_end + 2, // Because of my invarient in parser
        &conn->request
    );

    if (headerParsingRes != OK) {
        fprintf(stdout, "Parsing Error Encountered\n");
        handleParseError(headerParsingRes, conn);
        return;
    }

    // Since everyting is fine we can say header parsing completed
    fprintf(stdout, "Header parsing successfull\n");
    conn->state = READING_BODY;
    // Update parse offset
    conn->parse_offset += header_size;
}

void handleRead(connection_t* conn) {
    while (1) {
        // Read into remaining buffer
        fprintf(stdout, "Reading the request buffer\n");
        ssize_t valread = read(conn->fd, conn->read_buf + conn->read_len, conn->read_cap - conn->read_len);
        if (valread > 0) {
            conn->read_len += valread;
            if (conn->read_len == conn->read_cap) {
                // Reallocate buffer to double the size
                conn->read_cap *= 2; // Double the capacity

                if (conn->state == READING_HEADERS && conn->read_cap > MAX_HEADER_SIZE) {
                    handleParseError(MAX_HEADER_SIZE, conn);
                    break;
                }

                char* temp = NULL;
                temp = realloc(conn->read_buf, conn->read_cap);
                if (!temp) {
                    conn->state = CLOSING;
                    break;
                }
                conn->read_buf = temp;
            }
        }
        else if (valread == 0) {
            conn->state = CLOSING;
            break;
        }
        else if (errno == EWOULDBLOCK || errno == EAGAIN) {
            break;
        }
        else {
            conn->state = CLOSING;
            break;
        }
    }

    // Check for states and pass onto different handlers
    if (conn->state == CLOSING) {
        return;
    }
    else if (conn->state == READING_HEADERS) {
        fprintf(stdout, "Starting header parsing\n");
        handleHeaders(conn);
    }
}

// I will first write about when there is parse error state
void handleWrite(connection_t* conn) {
    conn->write_sent = send(conn->fd, conn->write_buf, conn->write_len, 0);
    fprintf(stdout, "Sent %zu bytes of data\n", conn->write_sent);
    conn->state = CLOSING;
}

uint32_t connectionHandler(connection_t* conn, u_int32_t events) {
    uint32_t closingSignal = -1;
    if (events & (EPOLLERR | EPOLLHUP)) {
        return closingSignal;
    }

    if (events & EPOLLIN) {
        handleRead(conn);
        if (conn->state == WRITING_RESPONSE) {
            fprintf(stdout, "Switching to write\n");
            return EPOLLOUT;
        }
        if (conn->state == CLOSING) {
            return closingSignal;
        }
        fprintf(stdout, "Starting read\n");
    }

    if (events & EPOLLOUT) {
        // Reading headers because in request loop I will go back to reading newRequest headers
        handleWrite(conn);
        if (conn->state == READING_HEADERS) {
            fprintf(stdout, "Switching to read\n");
            return EPOLLIN;
        }
        if (conn->state == CLOSING) {
            return closingSignal;
        }
    }
}
