#include <netinet/in.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "httpParser.h"

#define PORT 8080
#define BUFFER_SIZE 4096
#define MAX_REQUEST_LIMIT 16384 // This is what max request can be

typedef struct {
    parserResult_t result;
    int status_code;
    char* status_text;
} error_entry_t;

// Define your mapping once
error_entry_t error_table[] = {
    {BAD_REQUEST_LINE,400,"Bad Request"},
    {BAD_HEADER_SYNTAX,400,"Bad Header Syntax"},
    {INVALID_VERSION,505,"HTTP Version Not Supported"},
    {INVALID_CONTENT_LENGTH,400,"Invalid Content Length"},
    {BODY_NOT_ALLOWED,400,"Body not allowed"},
    {MISSING_REQUIRED_HEADERS,400,"Missing Required Headers"},
    {UNSUPPORTED_TRANSFER_ENCODING, 501,"Not Implemented"},
    {UNSUPPORTED_METHOD,405,"Method Not Allowed"},
    {HEADER_TOO_LARGE,431,"Request Header Fields Too Large"},
    {TOO_MANY_HEADERS,400,"Too Many Headers"},
    {PAYLOAD_TOO_LARGE,413,"Payload Too Large"}
};

void handleParseError(parserResult_t res, int socket) {
    int status = 400; // Default fallback
    char* msg = "Bad Request";

    // Loop through the table to find the specific error
    for (size_t i = 0; i < sizeof(error_table) / sizeof(error_table[0]); i++) {
        if (error_table[i].result == res) {
            status = error_table[i].status_code;
            msg = error_table[i].status_text;
            break;
        }
    }

    // One single send logic for everything
    char response[256];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        status, msg);

    send(socket, response, len, 0);
}

int main() {
    int server_fd, new_socket;
    int opt = 1;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "Socket creation failed\n");
        exit(EXIT_FAILURE);
    }

    // Setting socket options, specially port
    if ((setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) < 0) {
        fprintf(stderr, "Socket option setup failed\n");
        exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // Forcefully binding socket to port 8080
    if (bind(server_fd, (struct sockaddr*)&address, addrlen) < 0) {
        fprintf(stderr, "Socket bind error\n");
        exit(EXIT_FAILURE);
    }

    // Listen to the socket
    if ((listen(server_fd, 3)) < 0) {
        fprintf(stderr, "Error while listening\n");
        exit(EXIT_FAILURE);
    }

    // Accept new connection
    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen)) < 0) {
        fprintf(stderr, "Error while accepting\n");
        exit(EXIT_FAILURE);
    }

    size_t bufferSize = BUFFER_SIZE;
    char* buffer = malloc(bufferSize);
    if (buffer == NULL) {
        fprintf(stderr, "Buffer Malloc Failed\n");
        exit(EXIT_FAILURE);
    }

    header_t* headerArray = malloc(sizeof(header_t) * 100);

    ssize_t valread;
    size_t readOffset = 0;
    size_t parseOffset = 0;
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
            fprintf(stderr, "Read Error\n");
            break;
        }

        // Internal Loop to handle multiple requests in one read
        while (1) {
            //headerEnd -> protocol delimiter
            // headerBlockEnd -> parser boundary -> For line based CRLF parsing
            headerEnd = strstr(buffer + parseOffset, "\r\n\r\n");
            if (!headerEnd) break;

            // Parse request line and headers after we have complete header block
            char* headerBlockEnd = headerEnd + 2;
            // Passing start of current request, not the buffer start
            httpInfo_t httpInfo;
            parserResult_t parseResult = requestAndHeaderParser(buffer + parseOffset, headerBlockEnd, headerArray, &httpInfo);
            if (parseResult != OK) {
                handleParseError(parseResult, new_socket);
                goto cleanup;
            }

            size_t headerSize = (headerEnd - (buffer + parseOffset)) + 4;
            size_t totalRequestSize = headerSize + httpInfo.contentLength;

            if (totalRequestSize > bufferSize) {
                if (totalRequestSize > MAX_REQUEST_LIMIT) {
                    handleParseError(PAYLOAD_TOO_LARGE, new_socket);
                    goto cleanup;
                }

                char* new_buffer = realloc(buffer, totalRequestSize + 1);
                if (!new_buffer) {
                    const char* response = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
                    send(new_socket, response, strlen(response), 0);
                    goto cleanup;
                }
                buffer = new_buffer;
                bufferSize = totalRequestSize + 1; // Update your tracking variable
            }

            // Check for full body
            if (readOffset < parseOffset + totalRequestSize) break;

            // Here we will parse body once the function is written
            char* bodyStart = buffer + parseOffset + headerSize;
            parseResult = bodyParser(bodyStart, &httpInfo);
            if (parseResult != OK) {
                handleParseError(parseResult, new_socket);
                goto cleanup;
            }
            printf("Request Processed. Method: %.*s, Body Size: %zu\n", (int)httpInfo.method.len, httpInfo.method.data, httpInfo.contentLength);

            // ---- Temporary response for testing ----
            const char* response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
            send(new_socket, response, strlen(response), 0);

            parseOffset += totalRequestSize;
        }

        // Shift remaining bytes to the front to prevent buffer overflow
        size_t remaining = readOffset - parseOffset;
        if (remaining > 0 && parseOffset > 0) {
            memmove(buffer, buffer + parseOffset, remaining);
        }
        readOffset = remaining;
        parseOffset = 0;
    }

cleanup:
    free(headerArray);
    free(buffer);
    close(new_socket);
    close(server_fd);

    return EXIT_SUCCESS;
}