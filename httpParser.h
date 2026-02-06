#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include <stddef.h>

/**
 * Structure representing a view into a buffer without ownership
 * data: Pointer to the buffer data
 * len: Length of the buffer view
 */
typedef struct {
    const char* data;
    size_t len;
}bufferView_t;

/**
 * Structure representing an HTTP header key-value pair
 */
typedef struct {
    bufferView_t key;
    bufferView_t value;
}header_t;

/**
 * Structure containing parsed HTTP request information
 * method: HTTP method (GET, POST, etc.)
 * path: Request path/URI
 * version: HTTP version (e.g., HTTP/1.1)
 * headerCnt: Number of headers parsed
 * headers: Array of parsed headers
 * contentLength: Length of request body
 * contentType: Content-Type header value
 * isContentLengthSeen: Flag indicating if Content-Length was present
 * body: Request body data
 * isKeepAlive: Flag for persistent connection (1=keep-alive, 0=close)
 */
typedef struct {
    bufferView_t method;
    bufferView_t path;
    bufferView_t version;
    size_t headerCnt;
    header_t* headers;
    size_t contentLength;
    bufferView_t contentType;
    int isContentLengthSeen;
    bufferView_t body;
    int isKeepAlive;
}httpInfo_t;

/**
 * Enum representing parser result codes
 */
typedef enum {
    OK,
    BAD_REQUEST_LINE,
    BAD_HEADER_SYNTAX,
    INVALID_VERSION,
    INVALID_CONTENT_LENGTH,
    BODY_NOT_ALLOWED,
    MISSING_REQUIRED_HEADERS,
    UNSUPPORTED_TRANSFER_ENCODING,
    UNSUPPORTED_METHOD,
    HEADER_TOO_LARGE,
    TOO_MANY_HEADERS,
    PAYLOAD_TOO_LARGE,
    REQUEST_TIMEOUT
}parserResult_t;

/**
 * Parses HTTP request line and headers from buffer
 * @param buffer Pointer to the start of the request data
 * @param headerEnd Pointer to the end of headers (\r\n\r\n)
 * @param headerArray Pre-allocated array to store parsed headers
 * @param uninitializedHttpInfo Pointer to httpInfo_t structure to populate
 * @return parserResult_t indicating success (OK) or specific error
 */
parserResult_t requestAndHeaderParser(char* buffer, char* headerEnd, header_t* headerArray, httpInfo_t* uninitializedHttpInfo);

/**
 * Parses HTTP request body
 * @param bodyStart Pointer to the start of the body data
 * @param httpInfo Pointer to httpInfo_t structure to update with body info
 * @return parserResult_t indicating success (OK) or specific error
 */
parserResult_t bodyParser(char* bodyStart, httpInfo_t* httpInfo);

#endif