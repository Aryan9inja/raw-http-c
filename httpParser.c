#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "httpParser.h"

char* strstr_len(const char* haystack, const char* needle, size_t len) {
    size_t needle_len = strlen(needle);
    if (needle_len > len) return NULL;
    for (size_t i = 0; i <= len - needle_len; i++) {
        if (strncmp(&haystack[i], needle, needle_len) == 0) {
            return (char*)&haystack[i];
        }
    }
    return NULL;
}

httpInfo_t requestAndHeaderParser(char* buffer, char* headerEnd, header_t* headerArray) {
    httpInfo_t httpInfo = {
        .method = {NULL, 0},
        .path = {NULL, 0},
        .version = {NULL, 0},
        .headerCnt = 0,
        .headers = headerArray,
        .contentLength = 0,
        .contentType = {NULL,0},
    };

    // First line offset
    char* firstLineEnd = strstr(buffer, "\r\n");
    if (!firstLineEnd) {
        fprintf(stderr, "Malformed request\n");
        return httpInfo;
    }
    size_t firstLineLength = firstLineEnd - buffer;

    // ---- Parsing Request line ----

    // Parsing Method
    char* methodEnd = memchr(buffer, ' ', firstLineLength);
    if (!methodEnd) {
        fprintf(stderr, "Invalid HTTP request\n");
        return httpInfo;
    }
    size_t methodLength = methodEnd - buffer;
    httpInfo.method.data = buffer;
    httpInfo.method.len = methodLength;

    // Parsing Path
    firstLineLength -= methodLength;
    char* pathStart = methodEnd;
    while (pathStart[0] == ' ') {
        pathStart++;
        firstLineLength--;
    }
    char* pathEnd = memchr(pathStart, ' ', firstLineLength);
    if (!pathEnd) {
        fprintf(stderr, "No space b/w path and version\n");
        return httpInfo;
    }
    size_t pathLength = pathEnd - pathStart;
    httpInfo.path.data = pathStart;
    httpInfo.path.len = pathLength;

    firstLineLength -= pathLength;
    char* versionStart = pathEnd;
    while (versionStart[0] == ' ') {
        versionStart++;
        firstLineLength--;
    }
    size_t versionLength = firstLineEnd - versionStart;
    httpInfo.version.data = versionStart;
    httpInfo.version.len = versionLength;

    // ---- Parsing Headers ----
    char* headerStart = firstLineEnd + 2;
    if (headerStart >= headerEnd) {
        fprintf(stderr, "No headers\n");
        return httpInfo;
    }
    size_t headerCnt = 0;
    while (headerStart < headerEnd) {
        size_t remaining = headerEnd - headerStart;
        char* lineEnd = strstr_len(headerStart, "\r\n", remaining);

        // If we hit the empty line (\r\n\r\n), we are done
        if (!lineEnd || lineEnd == headerStart) break;

        size_t lineLength = lineEnd - headerStart;
        char* colon = memchr(headerStart, ':', lineLength);

        if (colon) {
            // Key is from start to colon
            size_t keyLen = colon - headerStart;
            httpInfo.headers[headerCnt].key.data = headerStart;
            httpInfo.headers[headerCnt].key.len = keyLen;

            // Value starts AFTER colon
            char* valStart = colon + 1;
            while (valStart < lineEnd && *valStart == ' ') valStart++;
            size_t valLen = lineEnd - valStart;

            httpInfo.headers[headerCnt].value.data = valStart;
            httpInfo.headers[headerCnt].value.len = valLen;

            if (keyLen == 12 && strncasecmp(headerStart, "Content-Type", keyLen) == 0) {
                httpInfo.contentType.data = valStart;
                httpInfo.contentType.len = lineEnd - valStart;
            }
            else if (keyLen == 14 && strncasecmp(headerStart, "Content-Length", keyLen) == 0) {
                size_t length = 0;
                for (size_t i = 0;i < valLen;i++) {
                    if (valStart[i] >= '0' && valStart[i] <= '9') {
                        length = length * 10 + (valStart[i] - '0');
                    }
                    else {
                        break;
                    }
                }
                httpInfo.contentLength = length;
            }

            headerCnt++;
        }

        headerStart = lineEnd + 2; // Jump to next line
    }
    httpInfo.headerCnt = headerCnt;

    return httpInfo;
}