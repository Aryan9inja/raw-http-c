#include "connection.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/errno.h>
#include <string.h>
#include <sys/socket.h>
#include "httpParser.h"
#include <sys/sendfile.h>
#include "handlers.h"

#define READ_BUFFER_SIZE 4096 // 4kb
#define MAX_HEADER_SIZE 8192 // 8kb
#define MIN_RESPONSE_BUFFER 65536

void initializeConnection(connection_t* conn, int fd, int fileRootFd) {
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
    conn->write_len = 0;
    conn->write_sent = 0;
    conn->write_buf = malloc(MIN_RESPONSE_BUFFER);
    if (!conn->write_buf) {
        perror("Malloc failed");
        conn->state = CLOSING;
        return;
    }
    conn->root_fd = fileRootFd;
    conn->file_fd = -1;
    conn->file_offset = 0;
    conn->file_remaining = 0;
    conn->shouldClose = 0;
}

void closeConnection(connection_t* conn) {
    close(conn->fd);
    if (conn->file_fd != conn->root_fd) {
        close(conn->file_fd);
    }
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
            header_end = conn->read_buf + i;
            break;
        }
    }

    if (!header_end) {
        // Wait for next EPOLLIN
        return;
    }
    // Calculate offset so that if later we realloc nothing changes
    conn->header_end = header_end - conn->read_buf;

    size_t header_size = conn->header_end - conn->parse_offset;
    if (header_size > MAX_HEADER_SIZE) {
        handleParseError(MAX_HEADER_SIZE, conn);
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
    conn->parse_offset += header_size + 4;
}

void handleBody(connection_t* conn) {
    char* bodyStart = conn->read_buf + conn->parse_offset;
    parserResult_t bodyParserRes = bodyParser(bodyStart, &conn->request);
    if (bodyParserRes == OK) {
        conn->state = PROCESSING;
    }
    else {
        conn->state = CLOSING;
        return;
    }
}

void handleRequestProcessing(connection_t* conn) {
    // decode url
    parserResult_t processingRes = OK;
    conn->request.decodedPath.len = 0;
    conn->request.normalizedPath.len = conn->request.normalizedPathCap;
    processingRes = decodeUrl(&conn->request.path, &conn->request.decodedPath);
    if (processingRes != OK) {
        handleParseError(processingRes, conn);
        return;
    }

    // normalize url
    processingRes = normalizePath(&conn->request.decodedPath, &conn->request.normalizedPath);
    if (processingRes != OK) {
        handleParseError(processingRes, conn);
        return;
    }

    // Request processing
    response_t generatedResponse = requestHandler(&conn->request, conn->root_fd);

    createWritableResponse(&generatedResponse, &conn->write_buf, &conn->write_len);
    if (!conn->request.isApi) {
        conn->file_fd = generatedResponse.fileDescriptor;
        conn->file_remaining = generatedResponse.fileSize;
        conn->file_offset = 0;
    }
    conn->shouldClose = generatedResponse.shouldClose;
    conn->state = WRITING_RESPONSE;
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
        return;
    }
    if (conn->state == READING_HEADERS) {
        handleHeaders(conn);
    }
    if (conn->state == READING_BODY) {
        handleBody(conn);
    }
    if (conn->state == PROCESSING) {
        handleRequestProcessing(conn);
    }
}

void handleSend(connection_t* conn) {
    while (conn->write_sent < conn->write_len) {
        ssize_t sent = send(conn->fd, conn->write_buf + conn->write_sent, conn->write_len - conn->write_sent, 0);
        if (sent < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                return;
            }
            conn->state = CLOSING;
            return;
        }
        conn->write_sent += sent;
    }
    // Reset for next request
    conn->write_sent=0;
    conn->write_len=0;

    if (!conn->request.isApi) {
        conn->state = SENDING_FILE;
        return;
    }

    // Check if we should close or keep-alive
    if (conn->shouldClose) {
        conn->state = CLOSING;
    } else {
        conn->state = READING_HEADERS;
    }
}

void handleFileSend(connection_t* conn) {
    off_t offset = conn->file_offset;
    size_t remainingFileSize = conn->file_remaining;
    while (remainingFileSize > 0) {
        size_t byteCount = remainingFileSize;
        ssize_t sent = sendfile(conn->fd, conn->file_fd, &offset, byteCount);

        if (sent <= 0) {
            if (errno == EAGAIN || errno == EINTR) {
                conn->file_offset = offset;
                conn->file_remaining = remainingFileSize;
                return;
            }
            conn->state = CLOSING;
            return;
        }

        remainingFileSize -= sent;
    }
    close(conn->file_fd);
    conn->file_fd = conn->root_fd;

    // Check if we should close or keep-alive
    if (conn->shouldClose) {
        conn->state = CLOSING;
    } else {
        conn->state = READING_HEADERS;
    }
}

// I will first write about when there is parse error state
void handleWrite(connection_t* conn) {
    if (conn->state == WRITING_RESPONSE) {
        handleSend(conn);
    }
    if (conn->state == SENDING_FILE) {
        handleFileSend(conn);
    }
}

uint32_t connectionHandler(connection_t* conn, u_int32_t events) {
    uint32_t closingSignal = UINT32_MAX;
    if (events & (EPOLLERR | EPOLLHUP)) {
        return closingSignal;
    }

    if (events & EPOLLIN) {
        handleRead(conn);
        if (conn->state == WRITING_RESPONSE) {
            return EPOLLOUT;
        }
        if (conn->state == CLOSING) {
            return closingSignal;
        }
    }

    if (events & EPOLLOUT) {
        // Reading headers because in request loop I will go back to reading newRequest headers
        handleWrite(conn);
        if (conn->state == READING_HEADERS) {
            return EPOLLIN;
        }
        if (conn->state == CLOSING) {
            return closingSignal;
        }
    }
}
