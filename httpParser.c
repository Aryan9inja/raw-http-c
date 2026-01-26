#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "httpParser.h"

httpInfo_t extractHttpInfo(char* httpString, char* headerEnd) {
    httpInfo_t httpInfo;

    int capacity = 10;
    char* saveptr = NULL;
    char* line = __strtok_r(httpString, "\r\n", &saveptr);
    char** lines = malloc(capacity * sizeof(char*));

    int lineCnt = 0;
    while (line != NULL) {
        lines[lineCnt] = line;
        printf("%s\n\n", lines[lineCnt]);
        line = __strtok_r(NULL, "\r\n", &saveptr);
        lineCnt++;
        if (lineCnt >= capacity) {
            capacity *= 2;
            lines = realloc(lines, capacity * sizeof(char*));
        }
    }
    lines[lineCnt] = NULL;

    saveptr = NULL;
    char* firstLineWord = __strtok_r(lines[0], " ", &saveptr);
    char* firstLineWords[3];

    printf("\n\nDebug\n\n");
    int firstLineWordCnt = 0;
    while (firstLineWord != NULL) {
        firstLineWords[firstLineWordCnt] = firstLineWord;
        printf("%s\n", firstLineWords[firstLineWordCnt]);
        firstLineWord = __strtok_r(NULL, " ", &saveptr);
        firstLineWordCnt++;
    }
    httpInfo.method = firstLineWords[0];
    httpInfo.path = firstLineWords[1];
    httpInfo.version = firstLineWords[2];

    printf("\n\nDebug2\n\n");
    httpInfo.headers = malloc(lineCnt * sizeof(header_t));

    int headerIdx = 0;
    int i = 1;
    while (lines[i] != NULL && lines[i]!=headerEnd) {
        char* headerSavePtr = NULL;

        char* key = __strtok_r(lines[i], ":", &headerSavePtr);
        char* value = __strtok_r(NULL, " ", &headerSavePtr);

        if (key && value) {
            printf("Header Found -> %s:%s\n", key, value);
            httpInfo.headers[headerIdx].key = key;
            httpInfo.headers[headerIdx].value = value;
            headerIdx++;
        }
        i++;
    }

    int contentLength = 0;
    for (int j = 0;j < headerIdx;j++) {
        if (httpInfo.headers[j].key != NULL && strcasecmp(httpInfo.headers[j].key, "Content-Length") == 0) {
            contentLength = atoi(httpInfo.headers[j].value);
            printf("Found Content-Length: %d\n", contentLength);
        }
    }

    return httpInfo;
}