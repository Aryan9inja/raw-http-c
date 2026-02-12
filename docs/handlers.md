# handlers.c Documentation

## Overview

`handlers.c` implements request routing, response generation, and transmission for the HTTP/1.1 server. It receives parsed requests from `httpParser.c`, matches them to appropriate handlers (API vs static files), generates responses with correct status codes and headers, and transmits responses back to clients using zero-copy `sendfile()` for files.

### v0.4 Status
Major enhancements for **static file serving** and **Content-Type support**:
- **Static File Handler**: Serves files from `public/` directory with zero-copy `sendfile()`
- **Content-Type Detection**: Automatic MIME type mapping based on file extensions
- **API vs File Routing**: Requests with `/api/` prefix handled separately from static files
- **Enhanced Error Responses**: 404 with body, 403 for forbidden files, 500 for server errors
- **Separate Send Paths**: `sendHeaders()`, `sendBody()`, `sendFileStream()` for optimized transmission
- Process model unchanged: Each child process serves files independently with no shared state

## Key Responsibilities

1. **Request Routing**: Route to API handlers or static file serving based on `/api/` prefix
2. **Static File Serving**: Serve files from `public/` directory with zero-copy `sendfile()`
3. **Content-Type Detection**: Determine MIME type from file extensions
4. **Response Generation**: Create response bodies for APIs or metadata for files
5. **Status Code Management**: Set appropriate HTTP status codes (200, 404, 403, 405, 500)
6. **Keep-Alive State Propagation**: Mirror connection state from request to response
7. **Response Formatting**: Format HTTP response headers with Content-Type and Content-Length
8. **Response Transmission**: Send headers + body/file via separate optimized paths
9. **Memory Management**: Allocate and free response bodies, close file descriptors

## Core Data Structures

### `response_t`
Complete response metadata with support for both malloc'd bodies and file descriptors:
```c
typedef struct {
    int statusCode;           // HTTP status code (200, 404, 403, etc.)
    char* statusText;         // Status message ("OK", "Not Found", etc.)
    char* body;              // Response body (malloc'd, for API responses)
    size_t bodyLen;          // Body length in bytes (for malloc'd bodies)
    int shouldClose;         // 1 = close connection, 0 = keep-alive
    int fileDescriptor;      // File descriptor for static files (-1 if none)
    size_t fileSize;         // File size in bytes (for sendfile)
    char* contentType;       // MIME type ("text/html", "text/css", etc.)
} response_t;
```

**New Fields (v0.4):**
- `fileDescriptor`: Set to open file FD for static files, -1 for API responses
- `fileSize`: File size from `fstat()`, used for Content-Length header
- `contentType`: MIME type string (literal), determines Content-Type header

**Memory Ownership:**
- `body`: Heap-allocated via `malloc()` if non-NULL (API responses)
- `statusText`: Points to string literal (no allocation)
- `contentType`: Points to string literal (no allocation)
- `fileDescriptor`: Opened by `fileHandler()`, closed by `sendResponse()`
- Caller must free `body` and close `fileDescriptor` after transmission (done in `sendResponse()`)

**Response Type Indicator:**
- If `fileDescriptor != -1`: Static file response (use `sendFileStream()`)
- If `body != NULL`: API response (use `sendBody()`)
- Mutually exclusive: never both set simultaneously

### `responseHeaders_t`
Placeholder for custom response headers:
```c
typedef struct {
    header_t* headers;
    int count;
} responseHeaders_t;
```

**Status**: Removed in v0.4. Content-Type header now sent via `response->contentType` field.

## Core Functions

### `initializeResponse()`
Creates a default `response_t` with safe initial values.

**Defaults:**
- `statusCode = (not set, will be set by handler)`
- `statusText = (not set, will be set by handler)`
- `body = NULL`
- `bodyLen = 0`
- `shouldClose = 0` (keep-alive)
- `fileDescriptor = -1` (no file)
- `fileSize = 0`
- `contentType = "text/plain"`

**Invariant**: Every response must start with these defaults before modification.

### `getFileType()`
Determines MIME type from file extension for Content-Type header.

**Signature:**
```c
void getFileType(char* relativePath, response_t* response);
```

**Algorithm:**
1. Find last `.` in filename using `strrchr()`
2. Extract extension (characters after `.`)
3. Match extension against known types (case-sensitive)
4. Set `response->contentType` to appropriate MIME type literal

**MIME Mapping Table:**
| Extension | Content-Type                 |
|-----------|------------------------------|
| `.html`   | `text/html`                  |
| `.css`    | `text/css`                   |
| `.js`     | `application/javascript`     |
| `.png`    | `image/png`                  |
| (none)    | `application/octet-stream`   |
| (other)   | `text/plain`                 |

**Example:**
```c
getFileType("index.html", &response);  // Sets contentType = "text/html"
getFileType("style.css", &response);   // Sets contentType = "text/css"
getFileType("README", &response);      // Sets contentType = "application/octet-stream"
```

**Limitations:**
- Case-sensitive matching (`.HTML` won't match)
- Limited type coverage (only 4 extensions + default)
- No charset parameter (always bare MIME type)

### `setNotFoundError()`
Sets 404 Not Found response with body.

**Signature:**
```c
void setNotFoundError(response_t* response);
```

**Response:**
- Status: 404 Not Found
- Body: "Route Not Found" (15 bytes, heap-allocated)
- Content-Type: Inherited (typically "text/plain")

**Used For:**
- Missing API routes
- Non-existent static files (ENOENT from `openat()`)

**malloc Failure Handling:**
Falls back to `setInternalServerError()` if body allocation fails.

### `setForbiddenFileRoute()`
Sets 403 Forbidden response for directory access or permission errors.

**Signature:**
```c
void setForbiddenFileRoute(response_t* response);
```

**Response:**
- Status: 403 Forbidden
- Body: "Forbidden file route" (20 bytes, heap-allocated)
- Content-Type: Inherited (typically "text/plain")

**Used For:**
- Directory access attempts (when `S_ISREG()` check fails)
- Permission errors (EACCES from `openat()`)

**malloc Failure Handling:**
Falls back to `setInternalServerError()` if body allocation fails.

### `setInternalServerError()`
Sets 500 Internal Server Error response (no body).

**Signature:**
```c
void setInternalServerError(response_t* response);
```

**Response:**
- Status: 500 Internal Server Error
- Body: NULL (0 bytes)
- Content-Type: Inherited

**Used For:**
- malloc/realloc failures
- `fstat()` failures
- Unexpected system call errors

**Rationale for No Body:**
Prevents cascading malloc failures when already in error state.

### `apiHandler()`
Handles API endpoints (routes with `/api/` prefix).

**Signature:**
```c
void apiHandler(response_t* response, httpInfo_t* httpInfo, apiRoutes_t route);
```

**Supported Routes:**

#### `ROUTE_ROOT` (`GET /api/`)
- **Response**: "Hello" (5 bytes)
- **Status**: 200 OK
- **Content-Type**: "text/plain"
- **Body Allocation**: `malloc(5)` for "Hello"

#### `ROUTE_ECHO` (`POST /api/echo`)
- **Response**: Echo of request body
- **Status**: 200 OK
- **Content-Type**: "text/plain"
- **Body Allocation**: `malloc(bodyLen)` for body copy
- **Body Copy**: Uses `memcpy()` to copy request body to response body

#### `ROUTE_NOT_FOUND` (Unknown API Path)
- **Response**: Delegates to `setNotFoundError()`
- **Status**: 404 Not Found
- **Body**: "Route Not Found" (15 bytes)

#### `ROUTE_UNKNOWN_METHOD` (Unsupported Method on API Route)
- **Response**: "This request method is currently unsupported" (44 bytes)
- **Status**: 405 Method Not Allowed
- **Content-Type**: "text/plain"

**Content-Type:**
All API responses use `"text/plain"` (hardcoded override at function start).

**malloc Failure Handling:**
All routes fall back to `setInternalServerError()` if body allocation fails.

### `fileHandler()`
Serves static files from `public/` directory using zero-copy `sendfile()`.

**Signature:**
```c
void fileHandler(response_t* response, httpInfo_t* httpInfo, int root_fd);
```

**Parameters:**
- `root_fd`: File descriptor for `public/` directory (opened at server startup)
- `httpInfo->normalizedPath`: Safe path after URL decoding and normalization

**Flow Diagram:**
```
┌─────────────────────────────────────┐
│ Strip leading '/' from normalized   │
│ path to create relative path        │
└──────────────┬──────────────────────┘
               │
               ▼
    ┌──────────────────────┐
    │ Path empty?          │
    └───┬──────────────┬───┘
   YES  │              │ NO
        ▼              ▼
    ┌────────────┐  ┌────────────────┐
    │ Set path = │  │ Use path as-is │
    │index.html  │  └────────┬───────┘
    └──────┬─────┘           │
           │                 │
           └────────┬────────┘
                    ▼
         ┌─────────────────────────┐
         │ openat(root_fd, path,   │
         │        O_RDONLY)         │
         └─────────┬───────────────┘
                   │
        ┌──────────┴──────────┐
   FAIL │                     │ SUCCESS
        ▼                     ▼
    ┌────────────┐      ┌──────────┐
    │ Check errno│      │ fstat(fd)│
    └─────┬──────┘      └────┬─────┘
          │                  │
    ┌─────┴──────┐      ┌────┴─────┐
    │ ENOENT?    │      │ S_ISREG? │
    │   → 404    │      └────┬─────┘
    │ EACCES?    │      YES  │  NO
    │   → 403    │           ▼       ▼
    │ Other?     │      ┌────────┐ ┌─────┐
    │   → 500    │      │ Get    │ │ 403 │
    └────────────┘      │ type   │ │Close│
                        └────┬───┘ └─────┘
                             ▼
                      ┌──────────────┐
                      │ Set response │
                      │ 200 OK       │
                      │ fileSize     │
                      │ fd (keep open)│
                      └──────────────┘
```

**Steps:**
1. **Path Processing:**
   - Strip leading `/` from `normalizedPath` to create relative path
   - Allocate temporary buffer for relative path string
   - Add null terminator for C string compatibility

2. **Default File Handling:**
   - If path is empty (`"/"`), realloc and set to `"index.html"`

3. **File Opening (Security Layer):**
   - Use `openat(root_fd, relativePath, O_RDONLY)` for safe path resolution
   - `openat()` ensures file is within `public/` directory (cannot escape via `..`)
   - Error handling based on errno:
     - `ENOENT/ENOTDIR`: File not found → 404
     - `EACCES`: Permission denied → 403
     - Other: Unexpected error → 500

4. **Content-Type Detection:**
   - Call `getFileType(relativePath, response)` to set MIME type

5. **File Metadata:**
   - Call `fstat(fd, &fileStat)` to get file size and type
   - Check `S_ISREG()`: Only regular files allowed (directories → 403)

6. **Response Setup:**
   - Set `statusCode = 200`, `statusText = "OK"`
   - Set `fileSize = fileStat.st_size` (for Content-Length)
   - Set `fileDescriptor = staticFile_fd` (kept open for `sendfile()`)

7. **Memory Management:**
   - Free `relativePath` buffer after use
   - File descriptor ownership transferred to `response` (closed in `sendResponse()`)

**Security:**
- `openat()` with base directory FD prevents path traversal
- `normalizePath()` (in parser) already rejected `..` escapes
- Directory access blocked (only regular files served)

**Error Mapping:**
| errno      | HTTP Status | Response Function        |
|------------|-------------|--------------------------|
| ENOENT     | 404         | `setNotFoundError()`     |
| ENOTDIR    | 404         | `setNotFoundError()`     |
| EACCES     | 403         | `setForbiddenFileRoute()` |
| (fstat err)| 500         | `setInternalServerError()` |
| (S_ISDIR)  | 403         | `setForbiddenFileRoute()` |
| (other)    | 500         | `setInternalServerError()` |

**Example Flows:**
```
GET / → index.html → 200 OK (820 bytes, text/html)
GET /style.css → style.css → 200 OK (913 bytes, text/css)
GET /missing.html → ENOENT → 404 Not Found
GET /../etc/passwd → (rejected by normalizePath) → 400 Bad Request
```

### `requestHandler()`
Routes requests to API handlers or static file serving based on `/api/` prefix.

**Signature:**
```c
response_t requestHandler(httpInfo_t* httpInfo, int root_fd);
```

**Parameters:**
- `httpInfo`: Parsed request with `isApi` flag set by `checkIfApi()`
- `root_fd`: File descriptor for `public/` directory (for file serving)

**Routing Logic:**
```
┌─────────────────────────────────────┐
│ Initialize response with defaults   │
│ Propagate keep-alive state          │
└──────────────┬──────────────────────┘
               │
               ▼
    ┌──────────────────────┐
    │ httpInfo->isApi?     │
    └───┬──────────────┬───┘
   YES  │              │ NO (Static Files)
        ▼              ▼
    ┌─────────────┐  ┌────────────────────┐
    │ API Routes  │  │ Method == "GET"?   │
    └──────┬──────┘  └───┬────────────┬───┘
           │        YES  │            │ NO
           ▼             ▼            ▼
    ┌──────────────┐  ┌──────────┐ ┌─────────────┐
    │ GET /api/?   │  │fileHandler│ │ 405 Method  │
    │   → "Hello"  │  │()         │ │Not Allowed  │
    │ POST /api/   │  └──────────┘ └─────────────┘
    │ echo?        │
    │   → Echo     │
    │ Other?       │
    │   → 404/405  │
    └──────────────┘
```

**API Routing (`isApi == 1`):**

**GET Requests:**
- Path `/`: Call `apiHandler(ROUTE_ROOT)` → "Hello"
- Other: Call `apiHandler(ROUTE_NOT_FOUND)` → 404

**POST Requests:**
- Path `/echo`: Call `apiHandler(ROUTE_ECHO)` → Echo body
- Other: Call `apiHandler(ROUTE_NOT_FOUND)` → 404

**Other Methods:**
- Call `apiHandler(ROUTE_UNKNOWN_METHOD)` → 405

**Static File Routing (`isApi == 0`):**

**GET Requests:**
- Any path: Call `fileHandler()` → Serve file from `public/`

**Other Methods:**
- Call `apiHandler(ROUTE_UNKNOWN_METHOD)` → 405

**Keep-Alive Propagation:**
```c
if (httpInfo->isKeepAlive == 0) response.shouldClose = 1;
```
Inverts keep-alive flag: `isKeepAlive=0` → `shouldClose=1`.

**Path Matching:**
- Uses `strncmp()` with exact length check
- Operates on `normalizedPath` (after URL decoding and sanitization)
- Path comparison is case-sensitive

**Return Value:**
Returns `response_t` struct (value semantics, not pointer).

### `sendHeaders()`
Sends HTTP status line and headers to client socket.

**Signature:**
```c
requestResponse_t sendHeaders(int socket, response_t* response, char* responseBuffer);
```

**Headers Sent:**
```
HTTP/1.1 <statusCode> <statusText>\r\n
Content-Length: <bodyLen or fileSize>\r\n
Content-Type: <contentType>\r\n
Connection: <keep-alive|close>\r\n
\r\n
```

**Content-Length Calculation:**
- If `fileDescriptor != -1`: Use `fileSize` (from `fstat()`)
- Otherwise: Use `bodyLen` (malloc'd body size)

**Connection Header:**
- `"close"` if `shouldClose == 1`
- `"keep-alive"` if `shouldClose == 0`

**Partial Write Handling:**
Implements retry loop for partial writes:
```c
while (remaining > 0) {
    ssize_t bytesSent = send(socket, responseBuffer + sentOffset, remaining, 0);
    if (bytesSent <= 0) {
        if (errno == EINTR) continue;  // Interrupted, retry
        return HEADER_SEND_ERROR;
    }
    remaining -= bytesSent;
    sentOffset += bytesSent;
}
```

**Return Values:**
- `Ok`: All headers sent successfully
- `HEADER_SEND_ERROR`: Send failed (network error, closed socket)

**Buffer Usage:**
Formats headers into provided `responseBuffer` (16KB), then sends.

### `sendBody()`
Sends malloc'd response body (for API responses).

**Signature:**
```c
requestResponse_t sendBody(int socket, response_t* response, char* responseBuffer);
```

**Steps:**
1. Copy `response->body` to `responseBuffer` with `memcpy()`
2. Send buffer contents to socket
3. Implement partial write retry loop (same as `sendHeaders()`)

**Return Values:**
- `Ok`: All body bytes sent successfully
- `BODY_SEND_ERROR`: Send failed

**When Used:**
Only called when `response->fileDescriptor == -1` (API responses, not files).

**Partial Write Handling:**
Same retry loop pattern as `sendHeaders()` (handles `EINTR`, tracks offset).

### `sendFileStream()`
Sends file contents using zero-copy `sendfile()` syscall.

**Signature:**
```c
requestResponse_t sendFileStream(int socket, response_t* response);
```

**Zero-Copy Mechanism:**
```c
off_t offset = 0;
size_t remainingFileSize = response->fileSize;
while (remainingFileSize > 0) {
    ssize_t sent = sendfile(socket, response->fileDescriptor, &offset, remainingFileSize);
    if (sent <= 0) {
        if (errno == EAGAIN || errno == EINTR) continue;
        return FILE_SEND_ERROR;
    }
    remainingFileSize -= sent;
}
```

**sendfile() Advantages:**
- **No User-Space Buffer**: File data transferred directly from kernel page cache to socket
- **Reduced System Calls**: Single `sendfile()` vs. multiple `read()` + `write()` pairs
- **CPU Efficiency**: Eliminates memory copy operations (zero-copy)
- **Page Cache Benefit**: Frequently accessed files served from cache (no disk I/O)

**Partial Transfer Handling:**
- Loop until all `fileSize` bytes sent
- Tracks `offset` (automatically updated by `sendfile()`)
- Handles `EAGAIN` (would block on non-blocking socket - not used currently)
- Handles `EINTR` (interrupted by signal)

**Return Values:**
- `Ok`: All file bytes sent successfully
- `FILE_SEND_ERROR`: `sendfile()` failed (socket closed, file error)

**When Used:**
Only called when `response->fileDescriptor != -1` (static file responses).

**Comparison to read+write Approach:**
```
read+write:  File → Kernel buffer → User buffer → Kernel buffer → Socket
sendfile:    File → Kernel buffer ─────────────────────────────→ Socket
```
Eliminates one kernel→user→kernel copy round-trip.

### `sendResponse()`
Master function that coordinates header and body/file transmission.

**Signature:**
```c
requestResponse_t sendResponse(int socket, response_t* response);
```

**Flow Diagram:**
```
┌──────────────────────────────┐
│ sendHeaders(socket, response)│
└──────────────┬───────────────┘
               │
        ┌──────┴──────┐
    SUCCESS          FAIL
        │              │
        ▼              ▼
    ┌──────────────┐  ┌───────────────┐
    │fileDescriptor│  │Return         │
    │!= -1?        │  │HEADER_SEND_   │
    └───┬──────┬───┘  │ERROR          │
   YES  │      │ NO   └───────────────┘
        ▼      ▼
    ┌────────┐ ┌────────────┐
    │sendFile│ │sendBody()  │
    │Stream()│ │            │
    └────┬───┘ └─────┬──────┘
         │           │
         └─────┬─────┘
               ▼
       ┌───────────────┐
       │ cleanup:      │
       │ - close(fd)   │
       │ - free(body)  │
       └───┬───────────┘
           ▼
       ┌─────────┐
       │ Return  │
       │ result  │
       └─────────┘
```

**Steps:**
1. **Send Headers**: Call `sendHeaders()` to transmit HTTP status and headers
   - On failure: Jump to cleanup, return error code

2. **Send Content** (conditional):
   - If `fileDescriptor != -1`: Call `sendFileStream()` (zero-copy file)
   - Otherwise: Call `sendBody()` (malloc'd buffer)
   - On failure: Jump to cleanup, return error code

3. **Cleanup** (always executed):
   - Close file descriptor if `fileDescriptor != -1`
   - Free body buffer if `bodyLen > 0`
   - Print status code to stdout

**Return Values:**
- `Ok`: Complete response sent successfully
- `HEADER_SEND_ERROR`: Header transmission failed
- `BODY_SEND_ERROR`: Body transmission failed
- `FILE_SEND_ERROR`: File transmission failed

**Error Handling:**
Uses `goto cleanup` pattern to ensure resource cleanup on error.

**Resource Management:**
- File descriptor closed even on error (prevents FD leak)
- Body buffer freed even on error (prevents memory leak)
- Cleanup guaranteed via goto label

**Status Code Logging:**
Prints `statusCode` to stdout for basic request logging (no timestamps or details).

**Invariant:**
Response resources (FD, body buffer) are always freed/closed exactly once.

## Keep-Alive State Machine (v0.2 Feature, Unchanged in v0.4)

### State Propagation Flow
```
httpParser.c:
  Parse Connection header → set httpInfo.isKeepAlive

handlers.c (requestHandler):
  Check httpInfo.isKeepAlive → set response.shouldClose

handlers.c (sendHeaders):
  Check response.shouldClose → send "Connection: close/keep-alive"

server.c:
  Check response.shouldClose → close connection or continue loop
```

### Connection Header Generation
```c
char* connectionString = response->shouldClose ? "close" : "keep-alive";
```

**Invariant**: Every response includes exactly one Connection header.

## Critical Invariants

### Response Invariants (Updated v0.4)
1. **Status Code Range**: Typically 200-599 (but not validated)
2. **Status Text**: Must be non-NULL string literal
3. **Body Allocation**: If `body != NULL`, must be heap-allocated (via `malloc()`)
4. **Body Length**: If `body != NULL`, `bodyLen` must match allocated size
5. **shouldClose Matches isKeepAlive**: `shouldClose = !isKeepAlive` (inverted logic)
6. **File XOR Body**: Either `fileDescriptor != -1` OR `body != NULL`, never both
7. **Content-Type Always Set**: Must point to valid MIME type string literal

### Memory Management Invariants (Updated v0.4)
1. **Response Body Ownership**: Handler allocates, `sendResponse()` frees
2. **File Descriptor Ownership**: `fileHandler()` opens, `sendResponse()` closes
3. **No Double Free**: Body freed exactly once, FD closed exactly once
4. **Cleanup on Error**: Resources freed/closed even if transmission fails

### Routing Invariants (Updated v0.4)
1. **Exact String Match**: Method and path must match exactly (length + content)
2. **API Prefix Check**: `/api/` prefix stripped before routing
3. **Default File Serving**: Non-API routes serve from `public/` directory
4. **GET-Only for Files**: Only GET method allowed for static files (others → 405)
5. **Method Check First**: Method validated before path routing

### Socket Invariants (Updated v0.4)
1. **Valid File Descriptor**: Socket FD must be open and writable
2. **Blocking Sockets**: All send operations block until complete (no timeout currently)
3. **Partial Write Handling**: All send functions retry until all bytes sent
4. **Error Recovery**: Send failures clean up resources but return error to caller

### File Serving Invariants (New in v0.4)
1. **Regular Files Only**: `S_ISREG()` check ensures only regular files served
2. **openat() Security**: Base directory FD prevents escaping `public/`
3. **FD Kept Open**: File remains open until after `sendfile()` completes
4. **Content-Length Accurate**: `fstat()` size matches actual file size at open time

## Known Limitations & Missing Features

### Routing & Methods

#### Limited Routes
- **Only 2 Routes**: `/` and `/echo`
- **No Wildcard Matching**: No pattern matching (e.g., `/users/*`)
- **No Route Parameters**: No extraction (e.g., `/users/:id`)
- **No Query Parameters**: Not parsed or extracted

#### Missing HTTP Methods
- **HEAD**: Required by HTTP/1.1 spec (should return headers only)
- **OPTIONS**: Needed for CORS and capability discovery
- **PUT**: Common for resource updates
- **DELETE**: Common for resource deletion
- **PATCH**: Common for partial updates
- **TRACE/CONNECT**: Less common but spec-defined

#### Method Handling Issues
- **405 Response**: No `Allow` header listing supported methods
- **OPTIONS Support**: Cannot discover supported methods
- **HEAD Handling**: Cannot get headers without body

### Response Generation

#### Missing Headers
- **Server**: No server identification header
- **Date**: Required by HTTP/1.1 for origin servers
- **Cache-Control**: No caching directives
- **ETag**: No cache validation
- **Last-Modified**: No modification time for static files
- **Location**: Cannot send redirects (3xx status)
- **Allow**: Not sent with 405 responses

#### Fixed in v0.4
- ✅ **Content-Type**: Now sent based on file extension or route type

#### Custom Headers
- **No Header API**: Cannot add arbitrary custom headers beyond Content-Type
- **Hardcoded Headers**: Only Content-Length, Content-Type, and Connection

#### Response Bodies
- **404/405 Now Have Bodies**: v0.4 adds descriptive error messages
- **No HTML Error Pages**: Error bodies are plain text, not user-friendly HTML
- **No JSON Support**: No structured data response format
- **Binary Support**: Works but requires client to interpret Content-Type

### Content Handling

#### Static File Serving (✅ Implemented in v0.4)
- **Status**: ✅ **Fully Implemented**
- **Features**: Zero-copy `sendfile()`, Content-Type detection, secure `openat()`
- **Limitations**: No directory listing, no range requests, no caching headers

#### Content-Type Detection (✅ Implemented in v0.4)
- **Status**: ✅ **Basic Implementation**
- **Coverage**: `.html`, `.css`, `.js`, `.png` + defaults
- **Limitations**: 
  - Limited MIME types (only 4 extensions)
  - No charset parameter (e.g., `text/html; charset=utf-8`)
  - Case-sensitive extension matching
  - No magic number detection (relies solely on extension)

#### Compression
- **Gzip/Deflate**: Not supported
- **Accept-Encoding**: Not checked
- **Content-Encoding**: Never sent

#### Multipart/Form Data
- **Parsing**: Not implemented (in parser)
- **Generation**: Not implemented
- **File Uploads**: Not supported

### Performance & Scalability

#### Buffer Management
- **Fixed Buffer Size**: 16KB response buffer (not configurable)
- **No Streaming for Dynamic Content**: API responses buffered entirely in memory
- **Files Use Zero-Copy**: ✅ v0.4 uses `sendfile()` for static files (no buffering)

#### Memory Efficiency
- **Body Duplication**: Echo handler still copies request body to response body
- **No Buffer Reuse**: Allocates new header buffer for each response (on stack)
- **File Serving Efficient**: ✅ v0.4 `sendfile()` requires no user-space buffer

#### Write Handling (✅ Improved in v0.4)
- **Partial Write Support**: ✅ All send functions now handle partial writes with retry loops
- **Blocking Sockets**: Still blocks on slow clients (no application-level timeout)
- **No Send Timeout**: System-level socket timeout only
- **Error Handling**: ✅ Cleanup guaranteed via goto cleanup pattern

### Protocol Compliance

#### HTTP/1.1 Violations
- **Missing Date Header**: Required for origin servers
- **Missing Host Validation**: Required (but checked in parser)
- **HEAD Method**: Required but not supported
- **OPTIONS Method**: Should be supported

#### Connection Management
- **No Timeout Headers**: Cannot send Keep-Alive timeout/max
- **No Connection Limits**: No max requests per connection
- **No Connection: upgrade**: Cannot upgrade to WebSocket, HTTP/2

#### Status Codes
- **Limited Set**: Only 200, 404, 405, 413, 431, 500, 501
- **No 3xx Redirects**: Cannot redirect
- **No 401/403**: No authentication/authorization
- **No 304**: No cache validation
- **No 206**: No partial content/range requests

### Security & Validation

#### Input Validation
- **No Path Sanitization**: Path used as-is
- **No Header Validation**: Response headers not validated
- **Body Size**: Limited only by buffer (16KB)

#### Security Headers
- **No X-Frame-Options**: Clickjacking protection
- **No X-Content-Type-Options**: MIME sniffing protection
- **No CSP**: Content Security Policy
- **No HSTS**: HTTP Strict Transport Security

### Error Handling

#### Write Errors
- **No Retry**: Write failure immediately returns
- **No Logging**: Errors not logged
- **Inconsistent State**: Failed write may leave partial response

#### malloc Failures
- **No Handling**: malloc failures cause NULL deref (except in caller)
- **No 500 Response**: Cannot send error response if malloc fails

#### Invalid Response State
- **No Validation**: Response not validated before sending
- **Buffer Overflow**: Not detected (relies on buffer size)

## Future Enhancements

### Core Features (Status: v0.4 Completed)
1. ✅ **Static File Serving**: Implemented with zero-copy `sendfile()` (v0.4)
2. ✅ **Content-Type Detection**: Basic MIME type mapping (v0.4)
3. **Directory Listing**: Auto-generate HTML index for directories (planned)
4. **Error Pages**: HTML error responses with styling (planned)
5. **Range Requests**: Partial content support with 206 status (planned)
6. **Caching Headers**: ETag, Last-Modified, Cache-Control for static files (planned)

### HTTP Methods
1. **HEAD Support**: Return headers without body (required by HTTP/1.1)
2. **OPTIONS Support**: List allowed methods, CORS headers
3. **PUT/DELETE**: Resource manipulation for REST APIs
4. **Method Routing**: Register custom handlers per method

### Response Headers
1. **Date Header**: Current timestamp (HTTP/1.1 required)
2. **Server Header**: Server identification and version
3. **Cache Headers**: ETag, Last-Modified, Cache-Control for static files
4. **Custom Headers API**: Allow arbitrary header addition

### Advanced Routing
1. **Pattern Matching**: Wildcard routes (e.g., `/files/*`)
2. **Route Parameters**: Extract path segments (e.g., `/users/:id`)
3. **Query Parameters**: Parse and expose query string
4. **Middleware**: Pre/post-processing hooks

### Performance (v0.5 Roadmap)
1. **Event-Driven Architecture**: Replace fork() with epoll event loop
2. **Non-Blocking I/O**: Async socket operations with state machines
3. **Connection Pooling**: Reuse resources instead of creating per request
4. **Buffer Pooling**: Reuse response buffers across requests
5. **Chunked Responses**: Send unknown-length responses incrementally

### Protocol Features
1. **Chunked Responses**: Send unknown-length responses (Transfer-Encoding: chunked)
2. **Compression**: gzip/deflate encoding for responses
3. **Range Requests**: Partial content (206 status) for video streaming
4. **Conditional Requests**: If-Modified-Since, If-None-Match (304 responses)

### Error Handling
1. **Graceful Degradation**: Handle errors without process crashes
2. **Error Logging**: Log all errors with timestamps and context
3. **Retry Logic**: Intelligent retry for transient failures
4. **Client Error Response**: Better error messages for debugging
