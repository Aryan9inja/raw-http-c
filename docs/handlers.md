# handlers.c Documentation

## Overview

`handlers.c` implements request routing, response generation, and transmission for the HTTP/1.1 server. It receives parsed requests from `httpParser.c`, matches them to appropriate handlers, generates responses with correct status codes and headers, and transmits responses back to clients.

### v0.3 Status
The request handlers are **unchanged from v0.2**. The architecture works seamlessly in the multi-process model:
- Each child process calls `requestHandler()` independently for its requests
- Response generation is stateless (no shared response pools or caches)
- Socket transmission is per-connection (each child process has its own client socket)
- No concurrency issues (process isolation eliminates data races)

## Key Responsibilities

1. **Request Routing**: Match HTTP method and path to handler functions
2. **Response Generation**: Create response bodies and metadata
3. **Status Code Management**: Set appropriate HTTP status codes
4. **Keep-Alive State Propagation**: Mirror connection state from request to response
5. **Response Formatting**: Format HTTP response headers and body
6. **Response Transmission**: Send formatted response to socket
7. **Memory Management**: Allocate and free response body buffers

## Core Data Structures

### `response_t`
Complete response metadata and body:
```c
typedef struct {
    int statusCode;           // HTTP status code (200, 404, etc.)
    char* statusText;         // Status message ("OK", "Not Found")
    char* body;              // Response body (malloc'd)
    size_t bodyLen;          // Body length in bytes
    int shouldClose;         // 1 = close connection, 0 = keep-alive
    responseHeaders_t headers; // UNUSED: Defined but not implemented
} response_t;
```

**Memory Ownership:**
- `body` is heap-allocated via `malloc()` if non-NULL
- `statusText` points to string literal (no allocation)
- `headers` field defined but never used
- Caller must free `body` after transmission (done in `sendResponse()`)

### `responseHeaders_t`
Placeholder for custom response headers:
```c
typedef struct {
    header_t* headers;
    int count;
} responseHeaders_t;
```

**Status**: Defined but **NOT** implemented. All responses use hardcoded headers.

## Core Functions

### `initializeResponse()`
Creates a default `response_t` with safe initial values.

**Defaults:**
- `statusCode = 200`
- `statusText = "OK"`
- `body = NULL`
- `bodyLen = 0`
- `shouldClose = 0` (keep-alive)
- `headers.headers = NULL`
- `headers.count = 0`

**Invariant**: Every response must start with these defaults before modification.

### `requestHandler()`
Routes requests to appropriate handlers and generates responses.

**Routing Logic:**
```
┌─────────────────────────────────────┐
│ Parse method from httpInfo          │
└──────────────┬──────────────────────┘
               │
               ▼
    ┌──────────────────────┐
    │ method == "GET"?     │
    └───┬──────────────┬───┘
        │ YES          │ NO
        ▼              ▼
    ┌─────────┐   ┌──────────────────┐
    │ GET     │   │ method == "POST"?│
    └────┬────┘   └───┬──────────┬───┘
         │            │ YES      │ NO
         ▼            ▼          ▼
    ┌─────────────┐  ┌─────┐  ┌─────────────┐
    │ path == "/" │  │POST │  │ 405 Method  │
    └──┬──────┬───┘  └──┬──┘  │Not Allowed  │
       │ YES  │ NO      │     └─────────────┘
       ▼      ▼         ▼
    ┌─────┐ ┌─────┐ ┌──────────────┐
    │ 200 │ │ 404 │ │path=="/echo"?│
    │Hello│ │     │ └──┬───────┬───┘
    └─────┘ └─────┘    │ YES   │ NO
                       ▼       ▼
                   ┌──────┐ ┌─────┐
                   │ 200  │ │ 404 │
                   │ Echo │ │     │
                   └──────┘ └─────┘
```

**Supported Routes:**

#### `GET /`
- **Response**: "Hello" (5 bytes)
- **Status**: 200 OK
- **Body Allocation**: `malloc(6)` for "Hello\0"

#### `POST /echo`
- **Response**: Echo of request body
- **Status**: 200 OK
- **Body Allocation**: `malloc(bodyLen + 1)` for body copy + null terminator
- **Body Copy**: Uses `memcpy()` to copy request body to response body

#### Unknown Path (Any Method)
- **Response**: Empty body
- **Status**: 404 Not Found
- **Body**: NULL

#### Unsupported Method (Any Path)
- **Response**: Empty body
- **Status**: 405 Method Not Allowed
- **Body**: NULL

**Keep-Alive Propagation:**
Every route checks `httpInfo->isKeepAlive`:
- If `isKeepAlive == 0`: Set `response.shouldClose = 1`
- If `isKeepAlive == 1`: Leave `response.shouldClose = 0` (default)

**Method Matching:**
Uses `strncmp()` with length check:
```c
strncmp(method.start, "GET", method.length) == 0 && method.length == 3
```

**Path Matching:**
Uses `strncmp()` with length check:
```c
strncmp(path.start, "/", path.length) == 0 && path.length == 1
```

### `sendResponse()`
Formats and transmits HTTP response to client socket.

**Response Format:**
```
HTTP/1.1 <statusCode> <statusText>\r\n
Content-Length: <bodyLen>\r\n
Connection: <keep-alive|close>\r\n
\r\n
<body>
```

**Steps:**
1. Allocate response buffer (`RESPONSE_BUFFER_SIZE = 16384` bytes)
2. Format status line: `"HTTP/1.1 %d %s\r\n"`
3. Add Content-Length header: Always sent, even for 0-length bodies
4. Add Connection header:
   - `"Connection: close\r\n"` if `shouldClose == 1`
   - `"Connection: keep-alive\r\n"` if `shouldClose == 0`
5. Add empty line: `"\r\n"`
6. Append body if present and within buffer
7. Send via `write()` to socket
8. Free response body if allocated
9. Free response buffer

**Buffer Overflow Handling:**
If headers + body exceeds `RESPONSE_BUFFER_SIZE`:
- Only headers are sent
- Body is silently dropped
- No error indication to client
- **Bug**: May violate Content-Length header value

**Memory Management:**
- Response buffer freed after `write()`
- Response body freed after `write()`
- Both freed even if `write()` fails

## Keep-Alive State Machine (v0.2 Feature)

### State Propagation Flow
```
httpParser.c:
  Parse Connection header → set httpInfo.isKeepAlive

handlers.c (requestHandler):
  Check httpInfo.isKeepAlive → set response.shouldClose

handlers.c (sendResponse):
  Check response.shouldClose → send "Connection: close/keep-alive"

server.c:
  Check response.shouldClose → close connection or continue loop
```

### Connection Header Generation
```c
if (response->shouldClose) {
    written += sprintf(responseBuffer + written, "Connection: close\r\n");
} else {
    written += sprintf(responseBuffer + written, "Connection: keep-alive\r\n");
}
```

**Invariant**: Every response includes exactly one Connection header.

## Critical Invariants

### Response Invariants
1. **Status Code Range**: Typically 200-599 (but not validated)
2. **Status Text**: Must be non-NULL string literal
3. **Body Allocation**: If `body != NULL`, must be heap-allocated (via `malloc()`)
4. **Body Length**: If `body != NULL`, `bodyLen` must match allocated size (minus null terminator)
5. **shouldClose Matches isKeepAlive**: `shouldClose = !isKeepAlive` (inverted logic)

### Memory Management Invariants
1. **Response Body Ownership**: Handler allocates, `sendResponse()` frees
2. **Response Buffer Ownership**: `sendResponse()` allocates and frees
3. **No Double Free**: Body freed exactly once (in `sendResponse()`)
4. **Null-Terminated Body**: Body buffer includes null terminator (for safety, not transmission)

### Routing Invariants
1. **Exact String Match**: Method and path must match exactly (length + content)
2. **First Match Wins**: Routes checked in order (GET / before POST /echo)
3. **Default 404**: Unknown paths return 404 (not 400)
4. **Method Check First**: Method validated before path routing

### Socket Invariants
1. **Valid File Descriptor**: Socket FD must be open and writable
2. **Blocking Write**: `write()` may block until all bytes sent
3. **No Partial Write Handling**: Assumes `write()` sends all bytes
4. **No Write Error Recovery**: Write failures leave connection in inconsistent state

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
- **Content-Type**: Never sent (clients must guess)
- **Server**: No server identification header
- **Date**: Required by HTTP/1.1 for origin servers
- **Cache-Control**: No caching directives
- **ETag**: No cache validation
- **Last-Modified**: No modification time
- **Location**: Cannot send redirects (3xx status)
- **Allow**: Not sent with 405 responses

#### Custom Headers
- **responseHeaders_t Unused**: Custom header support defined but not implemented
- **No Header API**: Cannot add arbitrary headers
- **Hardcoded Headers**: Only Content-Length and Connection

#### Response Bodies
- **Error Responses Have No Body**: 404, 405 send headers only
- **No HTML Error Pages**: Cannot send user-friendly error pages
- **No JSON Support**: No structured data responses
- **Binary Support**: Limited (no Content-Type indication)

### Content Handling

#### Static File Serving
- **Status**: Not implemented
- **Impact**: Cannot serve HTML, CSS, JS, images
- **Future**: Primary v0.2+ goal

#### Content-Type Detection
- **Status**: Not implemented
- **Impact**: Clients cannot determine response format
- **Workaround**: Clients must assume or guess content type

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
- **Buffer Overflow**: Silent body truncation if too large
- **No Streaming**: Entire response buffered in memory
- **No Chunked Response**: Cannot send large responses in chunks

#### Memory Efficiency
- **Body Duplication**: Echo copies request body to response body (unnecessary)
- **sprintf Inefficiency**: Uses sprintf for formatting (slower than direct writes)
- **No Buffer Reuse**: Allocates new buffer for each response

#### Write Handling
- **No Partial Write Support**: Assumes `write()` sends all bytes
- **Blocking Write**: Can block indefinitely if client slow
- **No Write Timeout**: No timeout for slow clients
- **No Error Recovery**: Write errors fatal (connection closed)

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

### Core Features
1. **Static File Serving**: Serve files from document root (v0.2 goal)
2. **Content-Type Detection**: MIME type based on file extension
3. **Directory Listing**: Auto-generate index for directories
4. **Error Pages**: HTML error responses with details

### HTTP Methods
1. **HEAD Support**: Return headers without body
2. **OPTIONS Support**: List allowed methods, CORS
3. **PUT/DELETE**: Resource manipulation
4. **Method Routing**: Register custom handlers per method

### Response Headers
1. **Content-Type**: Automatic detection and setting
2. **Date Header**: Current timestamp (HTTP/1.1 required)
3. **Server Header**: Server identification
4. **Cache Headers**: ETag, Last-Modified, Cache-Control
5. **Custom Headers API**: Allow arbitrary header addition

### Advanced Routing
1. **Pattern Matching**: Wildcard routes (e.g., `/files/*`)
2. **Route Parameters**: Extract path segments (e.g., `/users/:id`)
3. **Query Parameters**: Parse and expose query string
4. **Middleware**: Pre/post-processing hooks

### Performance
1. **Response Streaming**: Send large files in chunks
2. **Zero-Copy**: sendfile() for static files
3. **Buffer Pooling**: Reuse response buffers
4. **Partial Writes**: Handle incomplete write() calls

### Protocol Features
1. **Chunked Responses**: Send unknown-length responses
2. **Compression**: gzip/deflate encoding
3. **Range Requests**: Partial content (206 status)
4. **Conditional Requests**: If-Modified-Since, If-None-Match

### Error Handling
1. **Partial Write Recovery**: Retry or buffer remaining bytes
2. **malloc Failure Handling**: Send 500 error or close gracefully
3. **Error Logging**: Log all errors with context
4. **Graceful Degradation**: Handle errors without crashing
