#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "httpParser.h"

// Custom strstr that respects a maximum length instead of relying on null-terminator
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

// Case-insensitive string search with length limit
char* strcasestr_len(const char* haystack, size_t haystack_len, const char* needle) {
    if (!needle || !*needle) return (char*)haystack;

    size_t needle_len = strlen(needle);
    if (needle_len > haystack_len) return NULL;

    // Search through the buffer without exceeding haystack_len
    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (strncasecmp(&haystack[i], needle, needle_len) == 0) {
            return (char*)&haystack[i];
        }
    }
    return NULL;
}

// Initialize httpInfo structure with default values
httpInfo_t* initializeHttpInfo(httpInfo_t* httpInfo) {
    httpInfo->contentLength = 0;
    httpInfo->isContentLengthSeen = 0;
    httpInfo->isKeepAlive = 1;
    httpInfo->headerCnt = 0;
    httpInfo->isApi = 0;
    httpInfo->decodedPath.len = 0;
    return httpInfo;
}

void checkIfApi(httpInfo_t* httpInfo) {
    size_t pathLength = httpInfo->path.len;
    char* pathStart = httpInfo->path.data;
    if (pathLength >= 5 && strncmp(pathStart, "/api/", 5) == 0) {
        httpInfo->isApi = 1;
        httpInfo->path.data = pathStart + 4;
        httpInfo->path.len = pathLength - 4;
    }
    else if (pathLength == 4 && strncmp(pathStart, "/api", 4) == 0) {
        httpInfo->isApi = 1;
        httpInfo->path.len = 1;
    }
}

parserResult_t requestAndHeaderParser(char* buffer, char* headerEnd, header_t* headerArray, httpInfo_t* uninitializedHttpInfo) {
    httpInfo_t* httpInfo = initializeHttpInfo(uninitializedHttpInfo);

    // Find the end of the first line (request line)
    char* firstLineEnd = strstr(buffer, "\r\n");
    if (!firstLineEnd) {
        return BAD_REQUEST_LINE;
    }
    size_t firstLineLength = firstLineEnd - buffer;

    // ---- Parsing Request line ----

    // Extract HTTP method (e.g., GET, POST)
    char* methodEnd = memchr(buffer, ' ', firstLineLength);
    if (!methodEnd) {
        return BAD_REQUEST_LINE;
    }
    size_t methodLength = methodEnd - buffer;
    httpInfo->method.data = buffer;
    httpInfo->method.len = methodLength;

    // Extract request path/URI
    firstLineLength -= methodLength;
    char* pathStart = methodEnd;
    // Skip leading spaces
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

    // Extract and validate HTTP version
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

    // Parse each header line until we hit the empty line
    while (headerStart < headerEnd) {
        size_t remaining = headerEnd - headerStart;
        char* lineEnd = strstr_len(headerStart, "\r\n", remaining);

        // If we hit the empty line (\r\n\r\n), we are done
        if (!lineEnd || lineEnd == headerStart) break;

        // Split header into key and value at colon
        size_t lineLength = lineEnd - headerStart;
        char* colon = memchr(headerStart, ':', lineLength);

        if (!colon) {
            return BAD_HEADER_SYNTAX;
        }

        // Extract header key (name)
        size_t keyLen = colon - headerStart;
        httpInfo->headers[headerCnt].key.data = headerStart;
        httpInfo->headers[headerCnt].key.len = keyLen;

        // Extract header value (skip leading whitespace)
        char* valStart = colon + 1;
        while (valStart < lineEnd && *valStart == ' ') valStart++;
        if (valStart == lineEnd) {
            return BAD_HEADER_SYNTAX;
        }

        size_t valLen = lineEnd - valStart;

        httpInfo->headers[headerCnt].value.data = valStart;
        httpInfo->headers[headerCnt].value.len = valLen;

        // Handle special headers (Content-Type, Content-Length, Connection)
        if (keyLen == 12 && strncasecmp(headerStart, "Content-Type", keyLen) == 0) {
            httpInfo->contentType.data = valStart;
            httpInfo->contentType.len = lineEnd - valStart;
        }
        else if (keyLen == 14 && strncasecmp(headerStart, "Content-Length", keyLen) == 0) {
            // Prevent duplicate Content-Length headers
            if (httpInfo->isContentLengthSeen != 0) {
                return INVALID_CONTENT_LENGTH;
            }
            // Parse Content-Length value
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
        else if (keyLen == 10 && strncasecmp(headerStart, "Connection", keyLen) == 0) {
            // Check if connection should be closed
            if (strcasestr_len(valStart, valLen, "close") != NULL) {
                httpInfo->isKeepAlive = 0;
            }
        }
        headerCnt++;

        headerStart = lineEnd + 2; // Jump to next line
    }

    httpInfo->headerCnt = headerCnt;

    // Validate: GET requests should not have a body
    if (httpInfo->method.data[0] == 'G' && httpInfo->contentLength != 0) {
        printf("The content length is %zu\n", httpInfo->contentLength);
        return BODY_NOT_ALLOWED;
    }

    checkIfApi(httpInfo);

    return OK;
}

// Parse the request body (sets body view to point at body data)
parserResult_t bodyParser(char* bodyStart, httpInfo_t* httpInfo) {
    httpInfo->body.data = bodyStart;
    httpInfo->body.len = httpInfo->contentLength;
    return OK;
}

parserResult_t decodeUrl(bufferView_t* requestPath, bufferView_t* decodedPath) {
    size_t pathSize = requestPath->len;
    char* pathPointer = requestPath->data;

    for (size_t i = 0;i < pathSize;i++, decodedPath->len++) {
        // Ending with % or %H like
        if (pathPointer[i] == '%') {
            if (i + 2 >= pathSize) {
                return BAD_REQUEST_PATH;
            }
            // 
            else if (!isxdigit(pathPointer[i + 1]) || !isxdigit(pathPointer[i + 2])) {
                return BAD_REQUEST_PATH;
            }
            else {
                int high = isdigit(pathPointer[i + 1]) ? pathPointer[i + 1] - '0'
                    : toupper(pathPointer[i + 1]) - 'A' + 10;
                int low = isdigit(pathPointer[i + 2]) ? pathPointer[i + 2] - '0'
                    : toupper(pathPointer[i + 2]) - 'A' + 10;

                if (decodedPath->len >= requestPath->len) {
                    return BAD_REQUEST_PATH; // overflow guard
                }

                char converted = (char)(high * 16 + low);
                decodedPath->data[decodedPath->len] = converted;
                i += 2;
            }
        }
        else {
            decodedPath->data[decodedPath->len] = pathPointer[i];
        }
    }
    return OK;
}

parserResult_t normalizePath(bufferView_t* decodedPath, bufferView_t* normalizedPath) {
    char* root = normalizedPath->data;
    *root = '/';

    char* out = root + 1;                 // stack pointer
    char* src = decodedPath->data;
    char* end = decodedPath->data + decodedPath->len;
    char* out_end = normalizedPath->data + normalizedPath->len;

    while (src < end) {

        // Skip redundant slashes
        if (*src == '/') {
            src++;
            continue;
        }

        // Extract segment
        char* seg_start = src;
        while (src < end && *src != '/') {
            src++;
        }
        size_t seg_len = src - seg_start;

        // Ignore "."
        if (seg_len == 1 && seg_start[0] == '.') {
            continue;
        }

        // Handle ".."
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            if (out <= root + 1) {
                // Attempt to escape root
                return BAD_REQUEST_PATH;
            }

            // Pop previous segment
            out--;
            while (out > root + 1 && *(out - 1) != '/') {
                out--;
            }
            continue;
        }

        // Normal segment — push
        if (out > root + 1) {
            if (out + 1 >= out_end) {
                return BAD_REQUEST_PATH;
            }
            *out++ = '/';
        }

        if (out + seg_len >= out_end) {
            return BAD_REQUEST_PATH;
        }

        memcpy(out, seg_start, seg_len);
        out += seg_len;
    }

    // Finalize
    normalizedPath->len = out - normalizedPath->data;
    return OK;
}
