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

parserResult_t requestAndHeaderParser(char* buffer, char* headerEnd, header_t* headerArray, httpInfo_t* httpInfo) {
    // Setting isSeen to zero to check for multiple content length
    httpInfo->isContentLengthSeen = 0;
    // Setting content length to zero explicitly
    httpInfo->contentLength=0;
    // First line offset
    char* firstLineEnd = strstr(buffer, "\r\n");
    if (!firstLineEnd) {
        return BAD_REQUEST_LINE;
    }
    size_t firstLineLength = firstLineEnd - buffer;

    // ---- Parsing Request line ----

    // Parsing Method
    char* methodEnd = memchr(buffer, ' ', firstLineLength);
    if (!methodEnd) {
        return BAD_REQUEST_LINE;
    }
    size_t methodLength = methodEnd - buffer;
    httpInfo->method.data = buffer;
    httpInfo->method.len = methodLength;

    // Parsing Path
    firstLineLength -= methodLength;
    char* pathStart = methodEnd;
    while (pathStart[0] == ' ') {
        pathStart++;
        firstLineLength--;
    }
    char* pathEnd = memchr(pathStart, ' ', firstLineLength);
    if (!pathEnd) {
        return BAD_REQUEST_LINE;
    }
    size_t pathLength = pathEnd - pathStart;
    httpInfo->path.data = pathStart;
    httpInfo->path.len = pathLength;

    firstLineLength -= pathLength;
    char* versionStart = pathEnd;
    while (versionStart[0] == ' ') {
        versionStart++;
        firstLineLength--;
    }
    size_t versionLength = firstLineEnd - versionStart;
    httpInfo->version.data = versionStart;
    httpInfo->version.len = versionLength;
    if (strncmp(versionStart, "HTTP/1.1", versionLength) != 0) {
        return INVALID_VERSION;
    }

    // ---- Parsing Headers ----
    char* headerStart = firstLineEnd + 2;
    if (headerStart >= headerEnd) {
        return MISSING_REQUIRED_HEADERS;
    }
    size_t headerCnt = 0;
    httpInfo->headers = headerArray;
    while (headerStart < headerEnd) {
        size_t remaining = headerEnd - headerStart;
        char* lineEnd = strstr_len(headerStart, "\r\n", remaining);

        // If we hit the empty line (\r\n\r\n), we are done
        if (!lineEnd || lineEnd == headerStart) break;

        size_t lineLength = lineEnd - headerStart;
        char* colon = memchr(headerStart, ':', lineLength);

        if (!colon) {
            return BAD_HEADER_SYNTAX;
        }

        // Key is from start to colon
        size_t keyLen = colon - headerStart;
        httpInfo->headers[headerCnt].key.data = headerStart;
        httpInfo->headers[headerCnt].key.len = keyLen;

        // Value starts AFTER colon
        char* valStart = colon + 1;
        while (valStart < lineEnd && *valStart == ' ') valStart++;
        if (valStart == lineEnd) {
            return BAD_HEADER_SYNTAX;
        }

        size_t valLen = lineEnd - valStart;

        httpInfo->headers[headerCnt].value.data = valStart;
        httpInfo->headers[headerCnt].value.len = valLen;

        if (keyLen == 12 && strncasecmp(headerStart, "Content-Type", keyLen) == 0) {
            httpInfo->contentType.data = valStart;
            httpInfo->contentType.len = lineEnd - valStart;
        }
        else if (keyLen == 14 && strncasecmp(headerStart, "Content-Length", keyLen) == 0) {
            if (httpInfo->isContentLengthSeen != 0) {
                return INVALID_CONTENT_LENGTH;
            }
            size_t length = 0;
            for (size_t i = 0;i < valLen;i++) {
                if (valStart[i] >= '0' && valStart[i] <= '9') {
                    length = length * 10 + (valStart[i] - '0');
                }
                else if (valStart[i] != '\r') {
                    return INVALID_CONTENT_LENGTH;
                }
                else {
                    break;
                }
            }
            httpInfo->contentLength = length;
        }
        headerCnt++;

        headerStart = lineEnd + 2; // Jump to next line
    }

    httpInfo->headerCnt = headerCnt;

    // Return body not allowed in get request
    if (httpInfo->method.data[0] == 'G' && httpInfo->contentLength != 0) {
        printf("The content length is %zu\n",httpInfo->contentLength);
        return BODY_NOT_ALLOWED;
    }

    return OK;
}