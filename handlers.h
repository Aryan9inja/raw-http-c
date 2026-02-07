#ifndef HANDLERS_H
#define HANDLERS_H

#include "httpParser.h"
#include <stddef.h>

/**
 * Structure representing HTTP response headers
 * key: Header name
 * value: Header value
 */
typedef struct{
    char* key;
    char* value;
}responseHeaders_t;

/**
 * Structure representing an HTTP response
 * statusCode: HTTP status code (e.g., 200, 404)
 * statusText: Status description (e.g., "OK", "Not Found")
 * headers: Array of response headers
 * body: Response body content
 * bodyLen: Length of the response body
 * shouldClose: Flag indicating if connection should be closed
 */
typedef struct{
    int statusCode;
    char* statusText;
    responseHeaders_t* headers;
    char* body;
    size_t bodyLen;
    int shouldClose;
}response_t;

/**
 * Enum for request response status
 */
typedef enum {
    Ok
}requestResponse_t;

/**
 * Enum for api routes
 */
typedef enum{
    ROUTE_ROOT,
    ROUTE_ECHO,
    ROUTE_NOT_FOUND,
    ROUTE_UNKNOWN_METHOD,
}apiRoutes_t;

/**
 * Handles incoming HTTP requests and generates appropriate responses
 * @param httpInfo Pointer to parsed HTTP request information
 * @return response_t structure containing the HTTP response
 */
response_t requestHandler(httpInfo_t* httpInfo);

/**
 * Sends an HTTP response through a socket
 * @param socket File descriptor of the client socket
 * @param response Pointer to the response structure to send
 * @return requestResponse_t status of the send operation
 */
requestResponse_t sendResponse(int socket, response_t* response);

#endif