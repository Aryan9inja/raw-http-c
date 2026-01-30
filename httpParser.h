#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

typedef struct {
    const char* data;
    size_t len;
}bufferView_t;
typedef struct {
    bufferView_t key;
    bufferView_t value;
}header_t;

typedef struct {
    bufferView_t method;
    bufferView_t path;
    bufferView_t version;
    size_t headerCnt;
    header_t* headers;
    size_t contentLength;
    bufferView_t contentType;
    int isContentLengthSeen;
}httpInfo_t;

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
    PAYLOAD_TOO_LARGE
}parserResult_t;

parserResult_t requestAndHeaderParser(char* buffer, char* headerEnd, header_t* headerArray, httpInfo_t* httpInfo);

parserResult_t bodyParser(char* bodyStart, httpInfo_t* httpInfo);

#endif