#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

typedef struct {
    char* key;
    char* value;
}header_t;

typedef struct {
    char* method;
    char* path;
    char* version;
    header_t* headers;
}httpInfo_t;

httpInfo_t extractHttpInfo(char* httpString);

#endif