#include "handlers.h"
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <stdio.h>
#include <errno.h>

#define RESPONSE_BUFFER_SIZE 16384

// Initialize a response structure with default values
response_t initializeResponse() {
    response_t response = {
        .bodyLen = 0,
        .body = NULL,
        .shouldClose = 0,
        .fileSize = 0,
        .fileDescriptor = -1,
        .contentType = "text/plain"
    };
    return response;
}

void getFileType(char* relativePath, response_t* response) {
    // Find the last dot in the string
    char* dot = strrchr(relativePath, '.');

    // If no dot is found or it's the last character
    if (!dot || dot == relativePath + strlen(relativePath) - 1) {
        response->contentType = "application/octet-stream"; // Default
        return;
    }

    char* ext = dot + 1; // Move past the '.'

    // Compare and assign
    if (strcmp(ext, "html") == 0) response->contentType = "text/html";
    else if (strcmp(ext, "css") == 0) response->contentType = "text/css";
    else if (strcmp(ext, "js") == 0)  response->contentType = "application/javascript";
    else if (strcmp(ext, "png") == 0) response->contentType = "image/png";
    else response->contentType = "text/plain";
}

void setInternalServerError(response_t* response) {
    response->statusCode = 500;
    response->statusText = "Internal Server Error";
    response->bodyLen = 0;
}

void setNotFoundError(response_t* response) {
    response->statusCode = 404;
    response->statusText = "Not Found";
    response->bodyLen = 15;
    response->body = malloc(response->bodyLen);
    if (!response->body) {
        perror("Malloc failed");
        setInternalServerError(response);
        return;
    }
    memcpy(response->body, "Route Not Found", response->bodyLen);
}

void setForbiddenFileRoute(response_t* response) {
    response->statusCode = 403;
    response->statusText = "Forbidden";
    response->bodyLen = 20;
    response->body = malloc(response->bodyLen);
    if (!response->body) {
        perror("Malloc failed");
        setInternalServerError(response);
        return;
    }
    memcpy(response->body, "Forbidden file route", response->bodyLen);
}

void apiHandler(response_t* response, httpInfo_t* httpInfo, apiRoutes_t route) {
    // For now stick to text/plain
    response->contentType = "text/plain";
    switch (route) {
    case ROUTE_ROOT:
        response->statusCode = 200;
        response->statusText = "OK";
        response->bodyLen = 5;
        response->body = malloc(response->bodyLen);
        if (!response->body) {
            perror("Malloc failed");
            setInternalServerError(response);
            return;
        }
        memcpy(response->body, "Hello", response->bodyLen);
        break;

    case ROUTE_ECHO:
        response->statusCode = 200;
        response->statusText = "OK";
        response->bodyLen = httpInfo->body.len;
        response->body = malloc(response->bodyLen);
        if (!response->body) {
            perror("Malloc failed");
            setInternalServerError(response);
            return;
        }
        memcpy(response->body, httpInfo->body.data, response->bodyLen);
        break;

    case ROUTE_UNKNOWN_METHOD:
        response->statusCode = 405;
        response->statusText = "Method Not Allowed";
        response->bodyLen = 44;
        response->body = malloc(response->bodyLen);
        if (!response->body) {
            perror("Malloc failed");
            setInternalServerError(response);
            return;
        }
        memcpy(response->body, "This request method is currently unsupported", response->bodyLen);
        break;

    default:
        setNotFoundError(response);
        break;
    }
}

void fileHandler(response_t* response, httpInfo_t* httpInfo, int root_fd) {
    int staticFile_fd;
    struct stat fileStat;
    // Strip leading '/' from path
    // And create a short lived relative path buffer for openat()
    char* relativePath = NULL;
    relativePath = malloc(httpInfo->normalizedPath.len);
    if (!relativePath) {
        perror("Malloc failed");
        setInternalServerError(response);
        return;
    }
    // Copy after striping '/';
    memcpy(relativePath, httpInfo->normalizedPath.data + 1, httpInfo->normalizedPath.len - 1);
    // Add null terminator at end
    relativePath[httpInfo->normalizedPath.len - 1] = '\0';

    // Check for empty path
    // if empty serve index file
    if (strlen(relativePath) == 0) {
        char* temp = realloc(relativePath, 11);
        if (temp == NULL) {
            perror("Realloc failed");
            free(relativePath);
            setInternalServerError(response);
            return;
        }
        relativePath = temp;
        memcpy(relativePath, "index.html", 11);
    }

    if ((staticFile_fd = openat(root_fd, relativePath, O_RDONLY)) == -1) {
        perror("OpenAt failed");
        free(relativePath);
        if (errno == ENOENT || errno == ENOTDIR) {
            setNotFoundError(response);
        }
        else if (errno == EACCES) {
            setForbiddenFileRoute(response);
        }
        else {
            setInternalServerError(response);
        }
        return;
    }

    printf("Serving file: %s\n", relativePath);
    getFileType(relativePath, response);
    free(relativePath);

    if (fstat(staticFile_fd, &fileStat) == -1) {
        perror("fstat failed");
        close(staticFile_fd);
        setInternalServerError(response);
        return;
    }

    // check if the stat is for directory or file
    if (S_ISREG(fileStat.st_mode) == 0) {
        close(staticFile_fd);
        setForbiddenFileRoute(response);
        return;
    }

    response->statusCode = 200;
    response->statusText = "OK";
    response->fileSize = fileStat.st_size;
    response->fileDescriptor = staticFile_fd;
}

response_t requestHandler(httpInfo_t* httpInfo, int root_fd) {
    response_t response = initializeResponse();
    if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;

    char* normalizedPathData = httpInfo->normalizedPath.data;
    size_t normalizedPathLen = httpInfo->normalizedPath.len;

    // isApi flag handles /api routes
    if (httpInfo->isApi) {
        // Handle GET requests
        if (httpInfo->method.len == 3 && strncmp(httpInfo->method.data, "GET", httpInfo->method.len) == 0) {
            // Route: GET /
            if (normalizedPathLen == 1 && strncmp(normalizedPathData, "/", normalizedPathLen) == 0) {
                apiHandler(&response, httpInfo, ROUTE_ROOT);
            }
            else {
                apiHandler(&response, httpInfo, ROUTE_NOT_FOUND);
            }
        }
        // Handle POST requests
        else if (httpInfo->method.len == 4 && strncmp(httpInfo->method.data, "POST", httpInfo->method.len) == 0) {
            // Route: POST /echo - echoes back the request body
            if (normalizedPathLen == 5 && strncmp(normalizedPathData, "/echo", normalizedPathLen) == 0) {
                apiHandler(&response, httpInfo, ROUTE_ECHO);
            }
            else {
                apiHandler(&response, httpInfo, ROUTE_NOT_FOUND);
            }
        }
        else {
            // Unsupported HTTP method
            apiHandler(&response, httpInfo, ROUTE_UNKNOWN_METHOD);
        }
    }
    else {
        // Handle static files
        if (httpInfo->method.len == 3 && strncmp(httpInfo->method.data, "GET", httpInfo->method.len) == 0) {
            fileHandler(&response, httpInfo, root_fd);
        }
        else {
            apiHandler(&response, httpInfo, ROUTE_UNKNOWN_METHOD);
        }
    }

    return response;
}

requestResponse_t sendHeaders(int socket, response_t* response, char* responseBuffer) {
    char* connectionString = response->shouldClose ? "close" : "keep-alive";
    size_t contentLength = response->fileDescriptor != -1 ? response->fileSize : response->bodyLen;

    // Build HTTP response headers
    size_t headerLen = snprintf(responseBuffer, RESPONSE_BUFFER_SIZE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Content-Type: %s\r\n"
        "Connection: %s\r\n\r\n",
        response->statusCode, response->statusText, contentLength, response->contentType, connectionString);

    size_t sentOffset = 0;
    size_t remaining = headerLen;
    while (remaining > 0) {
        ssize_t bytesSent = send(socket, responseBuffer + sentOffset, remaining, 0);
        if (bytesSent <= 0) {
            if (errno == EINTR) continue;
            return HEADER_SEND_ERROR;
        }
        remaining -= bytesSent;
        sentOffset += bytesSent;
    }

    return Ok;
}

requestResponse_t sendBody(int socket, response_t* response, char* responseBuffer) {
    memcpy(responseBuffer, response->body, response->bodyLen);
    size_t responseLen = response->bodyLen;

    size_t sentOffset = 0;
    size_t remaining = responseLen;
    while (remaining > 0) {
        ssize_t bytesSent = send(socket, responseBuffer + sentOffset, remaining, 0);
        if (bytesSent <= 0) {
            if (errno == EINTR) continue;
            return BODY_SEND_ERROR;
        }
        remaining -= bytesSent;
        sentOffset += bytesSent;
    }
    return Ok;
}

requestResponse_t sendFileStream(int socket, response_t* response) {
    off_t offset = 0;
    size_t remainingFileSize = response->fileSize;
    while (remainingFileSize > 0) {
        size_t byteCount = remainingFileSize;
        ssize_t sent = sendfile(socket, response->fileDescriptor, &offset, byteCount);

        if (sent <= 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            return FILE_SEND_ERROR;
        }

        remainingFileSize -= sent;
    }
    return Ok;
}

requestResponse_t sendResponse(int socket, response_t* response) {
    requestResponse_t result = Ok;
    char responseBuffer[RESPONSE_BUFFER_SIZE];

    requestResponse_t headerResult = sendHeaders(socket, response, responseBuffer);
    if (headerResult != Ok) {
        fprintf(stdout, "Header sending failed");
        result = headerResult;
        goto cleanup;
    }

    if (response->fileDescriptor != -1) {
        requestResponse_t fileResult = sendFileStream(socket, response);
        if (fileResult != Ok) {
            fprintf(stdout, "File sending failed");
            result = fileResult;
            goto cleanup;
        }
    }
    else {
        requestResponse_t bodyResult = sendBody(socket, response, responseBuffer);
        if (bodyResult != Ok) {
            fprintf(stdout, "Body sending failed");
            result = bodyResult;
            goto cleanup;
        }
    }

cleanup:
    if (response->fileDescriptor != -1) {
        close(response->fileDescriptor);
    }
    if (response->bodyLen > 0) {
        free(response->body);
    }

    printf("%d",response->statusCode);
    return result;
}
