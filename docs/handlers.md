# handlers.c Documentation

## Overview

`handlers.c` implements request routing and response generation for the HTTP/1.1 server. It receives parsed requests from `httpParser.c`, matches them to appropriate handlers (API vs static files), and generates response data into buffers. **Handlers no longer send data directly to sockets** - all I/O is delegated to `connection.c`, enabling a clean separation between response generation and transmission.

### v0.5 Status
Major architectural refactoring for **event-driven architecture**:
- **Buffer-Based Interface**: Responses formatted into memory buffers, not sent directly
- **No Socket I/O in Handlers**: All `send()` and `sendfile()` calls moved to `connection.c`
- **Synchronous Handlers**: No blocking I/O operations - handlers are pure data generation
- **Simplified API**: Three functions replace old send functions: `createWritableResponse()`, `generateResponseHeaders()`, `addBody()`
- **Separation of Concerns**: Handler layer generates data, connection layer handles transmission
- **Static File Serving**: File descriptor passed to connection layer for zero-copy `sendfile()`
- **Content-Type Detection**: Automatic MIME type mapping based on file extensions (unchanged)
- **API vs File Routing**: Requests with `/api/` prefix handled separately from static files (unchanged)

## Key Responsibilities

1. **Request Routing**: Route to API handlers or static file serving based on `/api/` prefix
2. **Static File Serving**: Open files from `public/` directory and prepare file metadata
3. **Content-Type Detection**: Determine MIME type from file extensions
4. **Response Body Generation**: Create response bodies for API endpoints
5. **Status Code Management**: Set appropriate HTTP status codes (200, 404, 403, 405, 500)
6. **Keep-Alive State Propagation**: Mirror connection state from request to response
7. **Response Buffer Formatting**: Format HTTP headers and body into memory buffers
8. **Memory Management**: Allocate response bodies, prepare buffer for transmission (no socket I/O)

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

**Memory Ownership (v0.5 Updated):**
- `body`: Heap-allocated via `malloc()` if non-NULL (API responses) - freed by `createWritableResponse()`
- `statusText`: Points to string literal (no allocation)
- `contentType`: Points to string literal (no allocation)
- `fileDescriptor`: Opened by `fileHandler()`, closed by connection layer after `sendfile()`
- Response buffer allocated and managed by connection layer

**Response Type Indicator:**
- If `fileDescriptor != -1`: Static file response (connection layer uses `sendfile()`)
- If `body != NULL`: API response (formatted into buffer by `createWritableResponse()`)
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
   - Set `fileDescriptor = staticFile_fd` (kept open, passed to connection layer)

7. **Memory Management:**
   - Free `relativePath` buffer after use
   - File descriptor ownership transferred to `response` (closed by connection layer after `sendfile()`)

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
Returns `response_t` struct (value semantics, not pointer). Response struct passed to connection layer for I/O operations.

## Response Buffer Generation Functions (v0.5)

### `generateResponseHeaders()`
Formats HTTP status line and headers into a response buffer.

**Signature:**
```c
size_t generateResponseHeaders(response_t* response, char* responseBuffer);
```

**Headers Generated:**
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

**Buffer Usage:**
Formats headers into provided `responseBuffer` using `snprintf()` (max 65536 bytes).

**Return Value:**
Returns the length of the formatted headers (size_t). This length is used to position body data after headers.

**Key Change (v0.5):**
Does **not** send to socket - only formats into buffer. Connection layer handles actual `send()` call.

### `addBody()`
Appends response body to buffer after headers.

**Signature:**
```c
void addBody(response_t* response, char* responseBuffer);
```

**Operation:**
Copies `response->body` to `responseBuffer` using `memcpy()` for `bodyLen` bytes.

**Preconditions:**
- `responseBuffer` must point to position after headers (header length offset)
- `response->body` must be allocated and contain valid data
- `response->bodyLen` must match allocated body size

**Usage Pattern:**
```c
size_t headerLen = generateResponseHeaders(&response, buffer);
addBody(&response, buffer + headerLen);
```

**Key Change (v0.5):**
Does **not** send to socket - only copies data into buffer. Connection layer handles transmission.

### `createWritableResponse()`
Master function that creates a complete response buffer ready for transmission.

**Signature:**
```c
void createWritableResponse(response_t* response, char** responseBuffer, size_t* responseBufferLen);
```

**Parameters:**
- `response`: Response struct with status, body, and metadata
- `responseBuffer`: Pointer to buffer (pre-allocated by caller, typically 65536 bytes)
- `responseBufferLen`: Output parameter, receives total buffer length (headers + body)

**Operation Flow:**
```
┌──────────────────────────────┐
│ generateResponseHeaders()    │
│ → formats headers into buffer│
│ → returns headerLen          │
└──────────────┬───────────────┘
               ▼
   ┌───────────────────────────┐
   │ addBody()                 │
   │ → copies body after headers│
   └──────────────┬────────────┘
                  ▼
        ┌─────────────────────┐
        │ free(response->body)│
        │ → cleanup body alloc│
        └──────────┬──────────┘
                   ▼
   ┌──────────────────────────────┐
   │ Set responseBufferLen =      │
   │ headerLen + bodyLen          │
   └──────────────────────────────┘
```

**Memory Management:**
- **Frees** `response->body` after copying to buffer (single allocation, freed here)
- Buffer itself is managed by connection layer (not allocated/freed here)
- For file responses (`fileDescriptor != -1`), body is NULL and not copied

**Output:**
- `*responseBufferLen` set to total bytes in buffer (headers + body)
- `*responseBuffer` contains complete HTTP response ready to send

**Key Change (v0.5):**
Replaces `sendResponse()` - no socket I/O, only buffer preparation. Connection layer handles `send()` call with the populated buffer.

**Usage Example:**
```c
char responseBuffer[65536];
char* bufPtr = responseBuffer;
size_t responseLen;
createWritableResponse(&response, &bufPtr, &responseLen);
// Connection layer now sends responseLen bytes from responseBuffer
```

## Architecture Changes (v0.5)

### Separation of Concerns

**v0.4 Model (Direct Socket I/O):**
```
Handler Layer:
  ├─ Generate response metadata
  ├─ Send headers to socket (sendHeaders)
  ├─ Send body to socket (sendBody)
  └─ Send file to socket (sendFileStream)
```

**v0.5 Model (Buffer-Based):**
```
Handler Layer:
  ├─ Generate response metadata
  ├─ Format headers into buffer (generateResponseHeaders)
  ├─ Append body to buffer (addBody)
  └─ Return file descriptor for files

Connection Layer (connection.c):
  ├─ Receive buffer from handler
  ├─ Send buffer to socket (write)
  └─ Send file using FD (sendfile)
```

### Benefits of v0.5 Architecture

1. **Event-Driven Ready**
   - Handlers are synchronous (no blocking I/O)
   - Connection layer can use epoll/kqueue event loops
   - Non-blocking sockets possible without handler changes

2. **Clear Responsibility Boundaries**
   - Handler: Business logic and data generation
   - Connection: Socket I/O and network operations
   - No mixing of concerns

3. **Testability**
   - Handlers can be tested without sockets
   - Response generation independent of transmission
   - Mock buffers instead of mock sockets

4. **Flexibility**
   - Connection layer can optimize I/O strategy
   - Can switch between blocking/non-blocking without handler changes
   - Easy to add compression, encryption at connection layer

5. **Performance Opportunities**
   - Buffer pooling possible (managed by connection layer)
   - Batch writes possible (combine multiple buffers)
   - Zero-copy sendfile still available for files

### Handler to Connection Interface

**For API Responses (with body):**
```c
// Handler generates response
response_t response = initializeResponse();
// ... set status, body, etc.

// Format into buffer
char buffer[65536];
char* bufPtr = buffer;
size_t bufLen;
createWritableResponse(&response, &bufPtr, &bufLen);

// Connection layer sends
// write(socket, buffer, bufLen);
```

**For File Responses:**
```c
// Handler opens file
response_t response = initializeResponse();
response.fileDescriptor = open(...);
response.fileSize = stat.st_size;

// Format headers only
char buffer[65536];
size_t headerLen = generateResponseHeaders(&response, buffer);

// Connection layer sends headers + file
// write(socket, buffer, headerLen);
// sendfile(socket, response.fileDescriptor, NULL, response.fileSize);
```

### Migration Path

**Removed Functions (v0.4 → v0.5):**
- `sendResponse()` - Replaced by `createWritableResponse()` + connection layer send
- `sendHeaders()` - Replaced by `generateResponseHeaders()` (no socket I/O)
- `sendBody()` - Replaced by `addBody()` (no socket I/O)
- `sendFileStream()` - Moved to connection layer (handler only provides FD)

**New Functions (v0.5):**
- `generateResponseHeaders()` - Formats headers into buffer
- `addBody()` - Appends body to buffer
- `createWritableResponse()` - Complete buffer preparation

**Unchanged Functions:**
- `initializeResponse()` - Still creates default response
- `getFileType()` - Still detects Content-Type
- `setNotFoundError()`, `setForbiddenFileRoute()`, `setInternalServerError()` - Still set error states
- `apiHandler()` - Still generates API response bodies
- `fileHandler()` - Still opens files and prepares metadata
- `requestHandler()` - Still routes requests

## Keep-Alive State Machine (v0.2 Feature, Unchanged in v0.5)

### State Propagation Flow
```
httpParser.c:
  Parse Connection header → set httpInfo.isKeepAlive

handlers.c (requestHandler):
  Check httpInfo.isKeepAlive → set response.shouldClose

handlers.c (generateResponseHeaders):
  Check response.shouldClose → format "Connection: close/keep-alive" into buffer

connection.c:
  Check response.shouldClose → close connection or continue event loop
```

### Connection Header Generation
```c
char* connectionString = response->shouldClose ? "close" : "keep-alive";
```

**Invariant**: Every response includes exactly one Connection header in the formatted buffer.

## Critical Invariants

### Response Invariants (v0.5 Updated)
1. **Status Code Range**: Typically 200-599 (but not validated)
2. **Status Text**: Must be non-NULL string literal
3. **Body Allocation**: If `body != NULL`, must be heap-allocated (via `malloc()`)
4. **Body Length**: If `body != NULL`, `bodyLen` must match allocated size
5. **shouldClose Matches isKeepAlive**: `shouldClose = !isKeepAlive` (inverted logic)
6. **File XOR Body**: Either `fileDescriptor != -1` OR `body != NULL`, never both
7. **Content-Type Always Set**: Must point to valid MIME type string literal

### Memory Management Invariants (v0.5 Updated)
1. **Response Body Ownership**: Handler allocates, `createWritableResponse()` frees
2. **File Descriptor Ownership**: `fileHandler()` opens, connection layer closes after `sendfile()`
3. **No Double Free**: Body freed exactly once in `createWritableResponse()`
4. **Buffer Management**: Response buffer allocated and freed by connection layer
5. **Synchronous Operation**: Handlers never block on I/O - all socket operations in connection layer

### Routing Invariants (Updated v0.4, Unchanged v0.5)
1. **Exact String Match**: Method and path must match exactly (length + content)
2. **API Prefix Check**: `/api/` prefix stripped before routing
3. **Default File Serving**: Non-API routes serve from `public/` directory
4. **GET-Only for Files**: Only GET method allowed for static files (others → 405)
5. **Method Check First**: Method validated before path routing

### Buffer Invariants (New in v0.5)
1. **Buffer Size**: Response buffer is 65536 bytes (RESPONSE_BUFFER_SIZE)
2. **Header Position**: Headers always start at buffer offset 0
3. **Body Position**: Body starts immediately after headers (at headerLen offset)
4. **Total Size Tracking**: `responseBufferLen` = headerLen + bodyLen
5. **Buffer Reuse**: Connection layer may reuse buffers across requests (handlers don't manage)

### File Serving Invariants (v0.4, Unchanged v0.5)
1. **Regular Files Only**: `S_ISREG()` check ensures only regular files served
2. **openat() Security**: Base directory FD prevents escaping `public/`
3. **FD Kept Open**: File remains open until connection layer completes `sendfile()`
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
- **404/405 Now Have Bodies**: v0.4 adds descriptive error messages (unchanged in v0.5)
- **No HTML Error Pages**: Error bodies are plain text, not user-friendly HTML
- **No JSON Support**: No structured data response format
- **Binary Support**: Works but requires client to interpret Content-Type

### Content Handling

#### Static File Serving (✅ Implemented in v0.4, Architecture Updated v0.5)
- **Status**: ✅ **Fully Implemented**
- **Features**: File descriptor passed to connection layer, Content-Type detection, secure `openat()`
- **v0.5 Change**: Handler opens file and passes FD to connection layer instead of calling `sendfile()` directly
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

#### Buffer Management (v0.5 Updated)
- **Fixed Buffer Size**: 65536-byte response buffer (RESPONSE_BUFFER_SIZE, increased from 16KB)
- **Buffer Allocation**: Managed by connection layer, not handler layer
- **No Streaming for Dynamic Content**: API responses buffered entirely in memory (unchanged)
- **Files Use File Descriptors**: v0.5 passes FD to connection layer for zero-copy `sendfile()`

#### Memory Efficiency (v0.5 Updated)
- **Body Duplication**: Echo handler still copies request body to response body
- **Single Body Allocation**: Body freed immediately after copying to buffer in `createWritableResponse()`
- **Buffer Reuse**: Connection layer can reuse buffers across multiple requests
- **File Serving Efficient**: v0.5 uses file descriptor approach, connection layer handles zero-copy `sendfile()`

#### Write Handling (v0.5 Architecture Change)
- **No Socket I/O in Handlers**: All send operations moved to connection layer
- **Synchronous Handlers**: Handlers never block on I/O operations
- **Partial Write Support**: Handled by connection layer (not handler concern)
- **Event-Driven Ready**: Handler layer fully synchronous, enabling event loop in connection layer

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

#### Write Errors (v0.5 Updated)
- **Handler Layer**: No write errors (handlers don't perform I/O)
- **Connection Layer**: Handles all write failures and retries
- **Separation of Concerns**: ✅ Error handling isolated to connection layer

#### malloc Failures
- **No Handling**: malloc failures cause NULL deref (except in caller)
- **No 500 Response**: Cannot send error response if malloc fails

#### Invalid Response State
- **No Validation**: Response not validated before buffer generation
- **Buffer Overflow**: Not detected (relies on buffer size)

## Future Enhancements

### Core Features (v0.4 Completed, v0.5 Architectural Foundation)
1. **Static File Serving**: Implemented with file descriptor approach (v0.4/v0.5)
2. **Content-Type Detection**: Basic MIME type mapping (v0.4, unchanged v0.5)
3. **Buffer-Based Interface**: Event-driven architecture foundation (v0.5)
4. **Directory Listing**: Auto-generate HTML index for directories (planned)
5. **Error Pages**: HTML error responses with styling (planned)
6. **Range Requests**: Partial content support with 206 status (planned)
7. **Caching Headers**: ETag, Last-Modified, Cache-Control for static files (planned)

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

### Performance (✅ v0.5 Foundation Complete)
1. **Separation of Concerns**: Handler layer generates data, connection layer handles I/O (v0.5)
2. **Synchronous Handlers**: No blocking I/O in handler layer enables event-driven architecture (v0.5)
3. **Buffer-Based Interface**: Response data prepared in buffers, not sent directly (v0.5)
4. **Event-Driven I/O**: Connection layer uses epoll/kqueue for non-blocking operations (planned)
5. **Non-Blocking Sockets**: Async socket operations with state machines in connection layer (planned)
6. **Connection Pooling**: Reuse resources instead of creating per request (planned)
7. **Buffer Pooling**: Reuse response buffers across requests (partially ready - connection layer manages buffers)
8. **Chunked Responses**: Send unknown-length responses incrementally (planned)

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
