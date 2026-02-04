#include "handlers.h"
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>

#define RESPONSE_BUFFER_SIZE 16384

// Initialize a response structure with default values
response_t initializeResponse() {
    response_t response = {
        .bodyLen = 0,
        .body = NULL,
        .shouldClose = 0
    };
    return response;
}

response_t requestHandler(httpInfo_t* httpInfo) {
    response_t response = initializeResponse();

    // Handle GET requests
    if (httpInfo->method.len == 3 && strncmp(httpInfo->method.data, "GET", httpInfo->method.len) == 0) {
        // Route: GET /
        if (httpInfo->path.len == 1 && strncmp(httpInfo->path.data, "/", httpInfo->path.len) == 0) {
            response.statusCode = 200;
            response.statusText = "OK";
            response.bodyLen = 5;
            response.body = malloc(response.bodyLen);
            if (!response.body) {
                response.statusCode = 500;
                response.statusText = "Internal Server Error";
                response.bodyLen = 0;
                return response;
            }
            memcpy(response.body, "Hello", response.bodyLen);
            if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;
        }
        else {
            response.statusCode = 404;
            response.statusText = "Not Found";
            if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;
        }
    }
    // Handle POST requests
    else if (httpInfo->method.len == 4 && strncmp(httpInfo->method.data, "POST", httpInfo->method.len) == 0) {
        // Route: POST /echo - echoes back the request body
        if (httpInfo->path.len == 5 && strncmp(httpInfo->path.data, "/echo", httpInfo->path.len) == 0) {
            response.statusCode = 200;
            response.statusText = "OK";
            response.bodyLen = httpInfo->body.len;
            response.body = malloc(response.bodyLen);
            if (!response.body) {
                response.statusCode = 500;
                response.statusText = "Internal Server Error";
                response.bodyLen = 0;
                return response;
            }
            memcpy(response.body, httpInfo->body.data, response.bodyLen);
            if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;
        }
        else {
            response.statusCode = 404;
            response.statusText = "Not Found";
            if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;
        }
    }
    else {
        // Unsupported HTTP method
        response.statusCode = 405;
        response.statusText = "Method Not Allowed";
        if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;
    }

    return response;
}

requestResponse_t sendResponse(int socket, response_t* response) {
    char* responseBuffer = malloc(RESPONSE_BUFFER_SIZE);
    size_t responseLen = 0;
    
    // Determine connection header value
    char* connectionString = response->shouldClose ? "close" : "keep-alive";
    
    // Build HTTP response headers
    size_t headerLen = snprintf(responseBuffer, RESPONSE_BUFFER_SIZE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: %s\r\n\r\n",
        response->statusCode, response->statusText, response->bodyLen, connectionString);

    // Append body if present and fits in buffer
    if (response->bodyLen > 0 && (headerLen + response->bodyLen) < RESPONSE_BUFFER_SIZE) {
        memcpy(responseBuffer + headerLen, response->body, response->bodyLen);
        responseLen = headerLen + response->bodyLen;
    }
    else {
        responseLen = headerLen;
    }

    // Send response to client
    send(socket, responseBuffer, responseLen, 0);
    
    // Free allocated memory
    if (response->bodyLen > 0) {
        free(response->body);
    }
    free(responseBuffer);
    return Ok;
}