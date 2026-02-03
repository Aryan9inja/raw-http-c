#ifndef HANDLERS_H
#define HANDLERS_H

#include "httpParser.h"
#include <stddef.h>

typedef struct{
    char* key;
    char* value;
}responseHeaders_t;

typedef struct{
    int statusCode;
    char* statusText;
    responseHeaders_t* headers;
    char* body;
    size_t bodyLen;
}response_t;

typedef enum {
    Ok
}requestResponse_t;

requestResponse_t sendResponse(int socket, httpInfo_t* httpInfo);

#endif