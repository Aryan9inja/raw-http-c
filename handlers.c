#include "handlers.h"
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <stdio.h>

#define MAX_BODY_SIZE 8192
#define RESPONSE_BUFFER_SIZE 16384

requestResponse_t sendResponse(int socket, httpInfo_t* httpInfo) {
    response_t response;
    response.body = malloc(MAX_BODY_SIZE);

    if (httpInfo->method.len == 3 && strncmp(httpInfo->method.data, "GET", httpInfo->method.len) == 0) {
        if (httpInfo->path.len == 1 && strncmp(httpInfo->path.data, "/", httpInfo->path.len) == 0) {
            response.statusCode = 200;
            response.statusText = "OK";
            memcpy(response.body, "Hello", 5);
            response.bodyLen = 5;
        }
        else {
            response.statusCode = 404;
            response.statusText = "Not Found";
        }
    }
    else if (httpInfo->method.len == 4 && strncmp(httpInfo->method.data, "POST", httpInfo->method.len) == 0) {
        if (httpInfo->path.len == 5 && strncmp(httpInfo->path.data, "/echo", httpInfo->path.len) == 0) {
            response.statusCode = 200;
            response.statusText = "OK";
            size_t copyLen = (httpInfo->body.len < MAX_BODY_SIZE) ? httpInfo->body.len : MAX_BODY_SIZE;
            memcpy(response.body, httpInfo->body.data, copyLen);
            response.bodyLen = copyLen;
        }
        else {
            response.statusCode = 404;
            response.statusText = "Not Found";
        }
    }
    else {
        response.statusCode = 405;
        response.statusText = "Method Not Allowed";
    }

    char* responseBuffer = malloc(RESPONSE_BUFFER_SIZE);
    size_t responseLen = 0;
    size_t headerLen = snprintf(responseBuffer, RESPONSE_BUFFER_SIZE,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        response.statusCode, response.statusText, response.bodyLen);

    if (response.bodyLen > 0 && (headerLen + response.bodyLen) < RESPONSE_BUFFER_SIZE) {
        memcpy(responseBuffer + headerLen, response.body, response.bodyLen);
        responseLen = headerLen + response.bodyLen;
    }
    else {
        responseLen = headerLen;
    }

    send(socket, responseBuffer, responseLen, 0);
    free(response.body);
    free(responseBuffer);
    return Ok;
}