#include "connection.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/errno.h>
#include <string.h>

#define READ_BUFFER_SIZE 4096 // 4kb
#define MAX_HEADER_SIZE 8192 // 8kb

void initializeConnection(connection_t* conn, int fd) {
    conn->fd = fd;
    conn->state = READING_HEADERS;
    conn->header_end = NULL;
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
}

void handleHeaders(connection_t* conn) {
    // Check for CRLF to get to header end
    char* header_end = strstr(conn->read_buf + conn->parse_offset, "\r\n\r\n");
    if (!header_end) {
        // Wait for next EPOLLIN
        return;
    }
    // Calculate offset so that if later we realloc nothing changes
    conn->header_end = header_end - conn->read_buf;

    size_t header_size = conn->header_end - conn->parse_offset;
    if (header_size > MAX_HEADER_SIZE) {
        handleParseError(MAX_HEADER_SIZE, conn);
        if (conn->state == CLOSING) {
            closeConnection(conn);
            return;
        }
        return;
    }

    // parserResult_t requestAndHeaderParser()
    parserResult_t headerParsingRes = requestAndHeaderParser(
        conn->read_buf + conn->parse_offset,
        conn->read_buf + conn->header_end + 2, // Because of my invarient in parser
        &conn->request
    );

    if (headerParsingRes != OK) {
        handleParseError(headerParsingRes, conn);
        return;
    }

    // Since everyting is fine we can say header parsing completed
    conn->state = READING_BODY;
    // Update parse offset
    conn->parse_offset += header_size;
}

void handleRead(connection_t* conn) {
    while (1) {
        // Read into remaining buffer
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
        closeConnection(conn);
        return;
    }
    else if (conn->state == READING_HEADERS) {
        handleHeaders(conn);
    }
}

// I will first write about when there is parse error state
void handleWrite(connection_t* conn) {

}

void closeConnection(connection_t* conn) {
    close(conn->fd);
    free(conn->read_buf);
    free(conn->write_buf);
    free(conn);
}

uint32_t connectionHandler(connection_t* conn, u_int32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) {
        closeConnection(conn);
        return -1;
    }

    if (events & EPOLLIN) {
        handleRead(conn);
        if (conn->state == WRITING_RESPONSE) {
            return EPOLLOUT;
        }
        if (conn->state == CLOSING) {
            return -1;
        }
    }

    if (events & EPOLLOUT) {
        handleWrite(conn);
        // Reading headers because in request loop I will go back to reading newRequest headers
        if (conn->state == READING_HEADERS) {
            return EPOLLIN;
        }
        if (conn->state == CLOSING) {
            return -1;
        }
    }
}
