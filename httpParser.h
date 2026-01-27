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
}httpInfo_t;

httpInfo_t requestAndHeaderParser(char* buffer, char* headerEnd, header_t* headerArray);

#endif