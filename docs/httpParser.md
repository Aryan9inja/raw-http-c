# httpParser.c Documentation

## Overview

`httpParser.c` implements HTTP/1.1 and HTTP/1.0 request parsing with a zero-copy design. It extracts request lines, headers, and body boundaries from raw TCP buffers without allocating memory (except for path processing), returning non-owning views (`bufferView_t`) into the caller's buffer.

### v0.5 Status
**Event-Driven Architecture** - Parser fully integrated with connection state machine:
- **No Direct Socket I/O**: Parser works with `connection_t` structures, no socket operations
- **Buffer-Based Error Handling**: Error responses written to `connection->write_buf`, not sent directly
- **Embedded Path Buffers**: `decodedPathBuf` and `normalizedPathBuf` now part of `httpInfo_t` (stack arrays)
- **Embedded Headers Array**: `headers[100]` now part of `httpInfo_t` structure (no heap allocation)
- **Simplified API**: `requestAndHeaderParser()` no longer takes `headerArray` parameter
- **Centralized Error Handler**: `handleParseError()` moved from `server.c` to `httpParser.c`
- Zero-copy design maintained: Parser still doesn't allocate for headers/body views
- Event-driven model: Parser integrates with epoll-based connection state machine

### v0.4 Background
URL security and routing features introduced in v0.4:
- **URL Decoding**: Percent-encoding decoding (`%20` → space, `%2F` → `/`)
- **Path Normalization**: Resolves `.` and `..` segments, prevents directory traversal
- **API Detection**: Identifies `/api/` prefix and strips it for routing
- **HTTP/1.0 Support**: Accepts both HTTP/1.1 and HTTP/1.0 (disables keep-alive for 1.0)

## Design Philosophy

### Zero-Copy Parsing
- Parser **never allocates memory** (v0.5: uses embedded buffers in `httpInfo_t`)
- Returns `bufferView_t` structs pointing into caller's buffer
- Caller retains buffer ownership and lifetime management
- No string duplication or copying during parse
- Path processing uses embedded stack buffers (no heap allocation)

### Memory Safety Contract
1. **Parser does not own buffer**: Caller must ensure buffer remains valid
2. **Parsed views valid until buffer modification**: Any buffer realloc/memmove invalidates all `bufferView_t` pointers
3. **No null-termination guarantee**: Buffer views may point to middle of buffer (not null-terminated substrings)
4. **Read-only**: Parser never modifies input buffer

### Event-Driven Integration (v0.5)
- **Connection State Machine**: Parser works with `connection_t` structures
- **No Direct Socket I/O**: All I/O handled by epoll-based event loop
- **Buffer-Based Responses**: Error responses written to `write_buf`, not sent directly
- **Stateless Design**: Parser has no persistent state between calls
- **Async-Friendly**: Parse operations synchronous but integrate with async I/O model

## Core Data Structures

### `bufferView_t`
Non-owning view into a buffer region:
```c
typedef struct {
    char* start;      // Pointer to first byte
    size_t length;    // Number of bytes
} bufferView_t;
```

**Invariants:**
- `start` points into caller's buffer
- `length` represents exact byte count (may be 0)
- View may span binary data (not necessarily text)

### `header_t`
Key-value pair using buffer views:
```c
typedef struct {
    bufferView_t key;
    bufferView_t value;
} header_t;
```

**Properties:**
- Key and value are separate views
- Leading whitespace in value is trimmed
- Trailing whitespace in value is **NOT** trimmed
- Key is case-sensitive in storage (case-insensitive for special headers)

### `httpInfo_t`
Complete parsed HTTP request (v0.5 event-driven architecture):
```c
typedef struct {
    bufferView_t method;
    bufferView_t path;                      // Raw path from request line
    bufferView_t version;
    size_t headerCnt;                       // Number of parsed headers
    header_t headers[100];                  // Embedded headers array (v0.5)
    size_t contentLength;                   // 0 if no Content-Length header
    bufferView_t contentType;               // Empty if no Content-Type header
    int isContentLengthSeen;                // Internal flag for duplicate detection
    bufferView_t body;                      // Empty if no body
    int isKeepAlive;                        // 1 = keep-alive, 0 = close
    int isApi;                              // 1 if /api/ prefix, 0 otherwise
    bufferView_t decodedPath;               // URL-decoded path (points to decodedPathBuf)
    bufferView_t normalizedPath;            // Normalized safe path (points to normalizedPathBuf)
    char decodedPathBuf[PATH_BUFFER_CAP];   // Embedded buffer for decoded path (v0.5)
    char normalizedPathBuf[PATH_BUFFER_CAP]; // Embedded buffer for normalized path (v0.5)
    size_t decodedPathCap;                  // Capacity of decodedPathBuf
    size_t normalizedPathCap;               // Capacity of normalizedPathBuf
} httpInfo_t;
```

**Key Fields:**
- `headers[100]`: Embedded stack array (v0.5), no heap allocation needed
- `decodedPath`: Percent-decoded path (e.g., `/hello%20world` → `/hello world`)
  - Points to `decodedPathBuf` (embedded stack array, v0.5)
  - Length: Same or shorter than original path
- `normalizedPath`: Safe path after resolving `.` and `..` segments
  - Points to `normalizedPathBuf` (embedded stack array, v0.5)
  - Always starts with `/`, never escapes document root
- `isApi`: Flag indicating `/api/` prefix was found and stripped
  - 1: API route (path rewritten: `/api/foo` → `/foo`)
  - 0: Static file route

**Memory Ownership (v0.5 Changes):**
- **No heap allocation**: All buffers embedded in structure (stack arrays)
- **Headers array**: Embedded `headers[100]` array, max 100 headers per request
- **Path buffers**: Embedded `decodedPathBuf` and `normalizedPathBuf` arrays (8KB each)
- **Buffer views**: `decodedPath` and `normalizedPath` point to embedded buffers
- **All other views**: Still zero-copy (point into caller's read buffer)

## Key Functions

### `initializeHttpInfo()`
Initializes `httpInfo_t` with default values before parsing.

**Signature:**
```c
httpInfo_t* initializeHttpInfo(httpInfo_t* httpInfo);
```

**Defaults:**
- `contentLength = 0`
- `isContentLengthSeen = 0`
- `isKeepAlive = 1` (HTTP/1.1 default)
- `headerCnt = 0`
- `isApi = 0`
- `decodedPath.data = decodedPathBuf` (v0.5: points to embedded buffer)
- `decodedPath.len = 0`
- `decodedPathCap = PATH_BUFFER_CAP` (v0.5)
- `normalizedPath.data = normalizedPathBuf` (v0.5: points to embedded buffer)
- `normalizedPath.len = 0`
- `normalizedPathCap = PATH_BUFFER_CAP` (v0.5)
- Other views: Not initialized (set during parsing)

**Returns:** Pointer to initialized `httpInfo` (for chaining)

**v0.5 Changes:**
- Initializes `decodedPath.data` and `normalizedPath.data` to point to embedded buffers
- Sets capacity fields for embedded path buffers
- No heap allocation needed

**Invariant**: Must be called before every `requestAndHeaderParser()` call to ensure clean state.

### `requestAndHeaderParser()`
Parses HTTP request line and all headers from buffer.

**Signature (v0.5):**
```c
parserResult_t requestAndHeaderParser(char* buffer, char* headerEnd, httpInfo_t* uninitializedHttpInfo);
```

**Parameters:**
- `buffer`: Pointer to start of HTTP request in read buffer
- `headerEnd`: Pointer to end of headers (`\r\n\r\n` position)
- `uninitializedHttpInfo`: Pointer to `httpInfo_t` structure (must be initialized first)

**v0.5 Changes:**
- **Removed `headerArray` parameter**: Headers now stored in embedded `httpInfo->headers[100]` array
- Simpler function signature for event-driven architecture

**Parsing Steps:**
1. **Request Line**: Extract method, path, version (space-delimited)
2. **Version Validation**: Ensure "HTTP/1.1" or "HTTP/1.0"
3. **HTTP/1.0 Handling**: Set `isKeepAlive = 0` for HTTP/1.0 requests
4. **Header Loop**: Parse headers into embedded `headers[]` array until `\r\n\r\n`
5. **Special Headers**: Extract Content-Length, Content-Type, Connection
6. **Body Validation**: Verify GET requests have no body
7. **API Detection**: Call `checkIfApi()` to identify `/api/` routes

**Returns**: `parserResult_t` (OK on success, error code on failure)

### `checkIfApi()`
Detects `/api/` prefix and rewrites path for API routing (new in v0.4).

**Signature:**
```c
void checkIfApi(httpInfo_t* httpInfo);
```

**Logic:**
```
┌─────────────────────────────┐
│ Check path prefix           │
└──────────┬──────────────────┘
           │
    ┌──────┴──────┐
    │ Starts with │
    │ "/api/"?    │
    └──┬──────┬───┘
  YES  │      │ NO
       ▼      ▼
    ┌────┐  ┌──────┐
    │Set │  │isApi │
    │isApi│  │= 0   │
    │= 1 │  └──────┘
    │    │
    │Rewrite:│
    │/api/foo│
    │→ /foo  │
    └────────┘
```

**Path Rewriting Examples:**
- `/api/users` → `/users` (isApi=1, path adjusted)
- `/api/` → `/` (isApi=1, edge case: exactly "/api/" becomes "/")
- `/api` → (isApi=1, exactly "/api" with no trailing slash, path becomes "/" with len=1)
- `/notapi` → `/notapi` (isApi=0, no change)
- `/` → `/` (isApi=0, no change)

**Implementation:**
```c
if (pathLength >= 5 && strncmp(pathStart, "/api/", 5) == 0) {
    httpInfo->isApi = 1;
    httpInfo->path.data = pathStart + 4;  // Skip "/api"
    httpInfo->path.len = pathLength - 4;
}
else if (pathLength == 4 && strncmp(pathStart, "/api", 4) == 0) {
    httpInfo->isApi = 1;
    httpInfo->path.len = 1;  // Becomes "/"
}
```

**Side Effects:**
- Modifies `httpInfo->path` (pointer and length)
- Sets `httpInfo->isApi` flag
- Path rewrite is in-place (no allocation)

### `decodeUrl()`
Decodes percent-encoded URL characters (new in v0.4).

**Signature:**
```c
parserResult_t decodeUrl(bufferView_t* requestPath, bufferView_t* decodedPath);
```

**Parameters:**
- `requestPath`: Raw path from request line
- `decodedPath`: Output buffer (caller must pre-allocate `decodedPath->data` with size >= `requestPath->len`)

**Percent-Encoding Rules:**
- `%XX` where XX are hex digits → single byte with value 0xXX
- Examples: `%20` → space, `%2F` → `/`, `%3A` → `:`
- Case-insensitive hex: `%2f` and `%2F` both decode to `/`

**Algorithm:**
```
For each character in requestPath:
    If '%':
        Validate: next 2 chars exist and are hex digits
        Convert hex → byte: high*16 + low
        Write byte to decodedPath
        Skip next 2 chars
    Else:
        Copy character as-is
```

**Error Conditions:**
- `%` at end of string or `%X` (incomplete sequence)
- Invalid hex digits (e.g., `%ZZ`, `%2G`)
- Returns `BAD_REQUEST_PATH` on any validation failure

**Example Inputs/Outputs:**
```
/hello%20world        → /hello world
/path%2Fto%2Ffile     → /path/to/file
/%2e%2e%2fetc%2fpasswd → /../etc/passwd  (normalized later)
/hello%2               → BAD_REQUEST_PATH (incomplete)
/hello%ZZ              → BAD_REQUEST_PATH (invalid hex)
```

**Security Note:**
Decoding happens *before* normalization. Malicious paths like `/%2e%2e/etc/passwd` are decoded to `/../etc/passwd` then rejected by `normalizePath()`.

### `normalizePath()`
Resolves `.` and `..` segments to prevent directory traversal (new in v0.4).

**Signature:**
```c
parserResult_t normalizePath(bufferView_t* decodedPath, bufferView_t* normalizedPath);
```

**Parameters:**
- `decodedPath`: URL-decoded path (may contain `.` and `..`)
- `normalizedPath`: Output buffer (caller must pre-allocate `normalizedPath->data` with size >= `decodedPath->len`)

**Algorithm (Stack-based path resolution):**
```
Initialize: stack = ['/']

For each path segment (separated by '/'):
    If segment is '.':
        Ignore (current directory, no-op)
    Else if segment is '..':
        If stack is empty (would escape root):
            Return BAD_REQUEST_PATH
        Else:
            Pop previous segment from stack
    Else (normal segment):
        Push '/' and segment onto stack

Result: Stack contents = normalized path
```

**Flow Diagram:**
```
┌──────────────────┐
│ Start: stack=[/] │
└────────┬─────────┘
         │
    ┌────▼─────┐
    │For each  │
    │segment   │
    └────┬─────┘
         │
    ┌────┴────┐
    │ Type?   │
    └┬──┬──┬──┘
     │  │  │
   . │  ││ ││ .. │  │ normal
     ▼  ▼  ▼
   ┌──┐┌────┐┌──────┐
   │Skip││Pop ││Push  │
   └──┘│seg ││seg   │
       └┬───┘└───┬──┘
        │ (check │
        │  root) │
        ▼        ▼
    ┌───────┐ ┌──────┐
    │At root?│ │Add / │
    │→ ERROR │ │+seg  │
    └───────┘ └──────┘
```

**Example Transformations:**
```
Input                Output           Notes
──────────────────   ─────────────    ────────────────────
/a/b/c               /a/b/c           No change
/a/./b               /a/b             . ignored
/a/../b              /b               .. pops /a
/a/b/../../c         /c               Two pops
/../etc/passwd       BAD_REQUEST_PATH Escape attempt
/./a/./../b/./c      /b/c             Multiple . and ..
/                    /                Root unchanged
//a///b//            /a/b             Redundant / skipped
```

**Security Guarantees:**
1. **No Root Escape**: Attempting `..` when at `/` returns error
2. **Always Absolute**: Result always starts with `/`
3. **No Redundant Separators**: Multiple `/` collapsed to single
4. **Deterministic**: Same input always produces same output

**Edge Cases:**
- Empty path → malformed (not passed to this function)
- `/..` → `BAD_REQUEST_PATH`
- `/.` → `/`
- `/a/b/c/../../..` → `/` (valid, resolves to root)
- `/a/b/c/../../../..` → `BAD_REQUEST_PATH` (escapes root)

**Attack Prevention:**
```
Attack Path                    Decoded           Normalized        Result
────────────────────────────   ───────────────   ───────────────   ──────
/../../etc/passwd              /../../etc/passwd BAD_REQUEST_PATH  Blocked
/%2e%2e/%2e%2e/etc/passwd      /../etc/passwd    BAD_REQUEST_PATH  Blocked
/static/../../../etc/passwd    /static/../etc/.. BAD_REQUEST_PATH  Blocked
/./././../../../etc/passwd     /../../../etc/.   BAD_REQUEST_PATH  Blocked
```

### `bodyParser()`
Creates a buffer view for the request body based on `Content-Length`.

**Behavior:**
- Returns view starting at current buffer position
- View length equals `contentLength` from headers
- Does **NOT** validate that buffer contains full body
- Caller must ensure sufficient bytes available

**Invariant**: Only call after successful `requestAndHeaderParser()` and when `contentLength > 0`.

### `handleParseError()`
Formats HTTP error response into connection's write buffer (v0.5).

**Signature:**
```c
void handleParseError(parserResult_t res, connection_t* conn);
```

**Parameters:**
- `res`: Parser error result code (e.g., `BAD_REQUEST_LINE`, `INVALID_VERSION`)
- `conn`: Pointer to connection structure containing write buffer

**Behavior:**
- Maps `parserResult_t` error code to HTTP status code and message
- Formats complete HTTP error response (status line + headers + body)
- Writes response directly into `conn->write_buf`
- Updates `conn->write_len` with response size
- Does **NOT** send to socket (caller handles I/O via state machine)

**v0.5 Event-Driven Architecture:**
- Moved from `server.c` to `httpParser.c` for better encapsulation
- Changed from `int socket` to `connection_t* conn` parameter
- No direct socket I/O - response buffered for async sending
- Integrates with epoll-based connection state machine

**Error Response Format:**
```
HTTP/1.1 <status_code> <status_text>\r\n
Content-Type: text/plain\r\n
Content-Length: <body_length>\r\n
Connection: close\r\n
\r\n
<status_text>
```

**Example Error Mappings:**
- `BAD_REQUEST_LINE` → 400 Bad Request
- `INVALID_VERSION` → 505 HTTP Version Not Supported
- `PAYLOAD_TOO_LARGE` → 413 Payload Too Large
- `REQUEST_TIMEOUT` → 408 Request Timeout

### Utility Functions

#### `strstr_len()`
Length-bounded `strstr()` - searches for substring within first `n` bytes.

#### `strcasestr_len()`
Case-insensitive length-bounded substring search.

**Use Case**: Search for "close" in Connection header value without null-terminated strings.

## HTTP Protocol Support

### Version Enforcement (Updated v0.4)
- **HTTP/1.1 Accepted**: Primary version, default keep-alive behavior
- **HTTP/1.0 Accepted** (new in v0.4): For Apache Bench compatibility
  - Automatically sets `isKeepAlive = 0` (no persistent connections)
  - Otherwise parsed identically to HTTP/1.1
- **Exact Match Required**: "HTTP/1.1" or "HTTP/1.0" (case-sensitive)
- **No HTTP/2**: Not supported (different framing, binary protocol)
- **INVALID_VERSION**: Returned for any other version string

**Version Detection Code:**
```c
if (strncmp(versionStart, "HTTP/1.1", 8) != 0 &&
    strncmp(versionStart, "HTTP/1.0", 8) != 0) {
    return INVALID_VERSION;
}

if (strncmp(versionStart, "HTTP/1.0", 8) == 0) {
    httpInfo->isKeepAlive = 0;  // HTTP/1.0 closes after response
}
```

**Rationale for HTTP/1.0 Support:**
- Apache Bench (`ab`) tool uses HTTP/1.0 by default
- Enables benchmarking without custom configuration
- Minimal implementation cost (single flag change)

## URL Processing Pipeline (New in v0.4)

Complete request path processing from raw URL to safe file path:

```
┌─────────────────────────────────┐
│ Raw Request: GET /api%2Fhello   │
│              HTTP/1.1            │
└──────────────┬──────────────────┘
               │
               ▼
┌──────────────────────────────────┐
│ 1. parseRequest() extracts path  │
│    httpInfo->path = "/api%2Fhello"│
└──────────────┬───────────────────┘
               │
               ▼
┌──────────────────────────────────┐
│ 2. decodeUrl() percent-decodes   │
│    decodedPath = "/api/hello"    │
└──────────────┬───────────────────┘
               │
               ▼
┌──────────────────────────────────┐
│ 3. checkIfApi() detects /api/    │
│    isApi = 1                      │
│    path rewritten = "/hello"      │
└──────────────┬───────────────────┘
               │
               ▼
┌──────────────────────────────────┐
│ 4. normalizePath() resolves . ..  │
│    normalizedPath = "/hello"      │
└──────────────┬───────────────────┘
               │
               ▼
┌──────────────────────────────────┐
│ 5. Handler routes based on:       │
│    - isApi flag                   │
│    - normalizedPath               │
└───────────────────────────────────┘
```

### Processing Order Importance

**Correct Order** (implemented):
```
Raw → Decode → Normalize → Route
```

**Why This Order:**
1. **Decode First**: Attackers can encode `.` and `/` to bypass checks
   - `%2e%2e` → `..` must be decoded before normalization
2. **Normalize Second**: Clean path for consistent routing
3. **Route Last**: Operate on safe, canonical paths

**Anti-Pattern** (insecure):
```
Raw → Normalize → Decode  ❌ WRONG
```
Problem: `/%2e%2e/etc` would pass normalization, decode to `/../etc`

### Security Layering

**Defense in Depth (Multiple Barriers):**
1. **URL Decoding Validation**: Rejects malformed percent-encoding
2. **Path Normalization**: Rejects `..` escape attempts
3. **openat() with Base FD**: Kernel-level path containment
4. **Regular File Check**: Blocks directory access

**Any single layer can prevent attacks, but all layers present:**

```
Attack: /%2e%2e/etc/passwd

Layer 1: decodeUrl() → /../etc/passwd (passes, valid encoding)
Layer 2: normalizePath() → BAD_REQUEST_PATH (blocks, .. at root)
         ✅ BLOCKED HERE

Even if Layer 2 failed:
Layer 3: openat(docroot_fd, "../etc/passwd") → ENOENT (stays in docroot)
         ✅ BLOCKED HERE TOO
```

## Security Invariants (New in v0.4)

### Path Safety Guarantees
1. **No Directory Traversal**: `normalizePath()` prevents escaping document root
2. **Canonical Form**: Same logical path always produces same normalized path
3. **Validation Before Use**: Errors returned before path reaches filesystem
4. **Attack Surface Reduced**: Rejects malformed input early in pipeline

### URL Decoding Invariants
1. **Strict Validation**: Invalid percent-encoding always rejected
2. **No Partial Decoding**: All-or-nothing (error if any `%XX` is malformed)
3. **Length Bound**: Decoded path ≤ original path length (never grows)
4. **Deterministic**: Same input always produces same output

### Path Normalization Invariants
1. **Root Anchored**: Result always starts with `/`
2. **No Escape**: Result path never leaves document root (no leading `..`)
3. **Idempotent**: Normalizing an already-normalized path returns same result
4. **No Redundancy**: No `.`, `..`, or redundant `/` in output

### API Detection Invariants
1. **Prefix Match Only**: Only `/api/` and `/api` trigger API routing
2. **Path Rewrite Consistent**: Strip exactly 4 characters (`/api`)
3. **Non-API Default**: Missing `/api/` prefix → static file serving

### Memory Safety
**v0.5 Changes:**
1. **No Dynamic Allocation**: Path buffers embedded in `httpInfo_t` (stack arrays)
2. **Buffer Size**: `decodedPathBuf` and `normalizedPathBuf` are 8KB each (`PATH_BUFFER_CAP`)
3. **No Freeing Required**: All memory managed as part of structure lifetime
4. **No Buffer Overrun**: Length checks prevent writing past buffer end

**v0.4 Background:**
- Previously: `decodedPath` and `normalizedPath` were heap-allocated
- Previously: Caller had to free path buffers after use

## Keep-Alive Semantics (v0.2 Feature, HTTP/1.0 Update in v0.4)

#### Default Behavior
- **HTTP/1.1**: `isKeepAlive` initialized to `1` (persistent connections)
- **HTTP/1.0**: `isKeepAlive` set to `0` (close after response)

#### Connection Header Processing
- **Case-Insensitive Search**: Uses `strcasestr_len()` to find "close" substring
- **Close Detection**: Any occurrence of "close" sets `isKeepAlive = 0`
- **Keep-Alive Explicit**: "Connection: keep-alive" not required for HTTP/1.1 (default)

**Examples:**
```
HTTP/1.1 + Connection: close           → isKeepAlive = 0
HTTP/1.1 + Connection: Close           → isKeepAlive = 0
HTTP/1.1 + Connection: keep-alive      → isKeepAlive = 1
HTTP/1.1 + Connection: upgrade, close  → isKeepAlive = 0
HTTP/1.1 + (no Connection header)      → isKeepAlive = 1
HTTP/1.0 + (any Connection header)     → isKeepAlive = 0 (version override)
```

#### Default Behavior
- `isKeepAlive` initialized to `1` (true)
- HTTP/1.1 assumes persistent connections unless explicitly closed

#### Connection Header Processing
- **Case-Insensitive Search**: Uses `strcasestr_len()` to find "close" substring
- **Close Detection**: Any occurrence of "close" sets `isKeepAlive = 0`
- **Keep-Alive Explicit**: "Connection: keep-alive" not required (default behavior)

**Examples:**
```
Connection: close                → isKeepAlive = 0
Connection: Close                → isKeepAlive = 0
Connection: keep-alive           → isKeepAlive = 1
Connection: upgrade, close       → isKeepAlive = 0
(no Connection header)           → isKeepAlive = 1
```

**Limitation**: Substring match can cause false positives (e.g., "Connection: enclose" would match "close").

### Content-Length Handling

#### Parsing Rules
- **Numeric Only**: Must be valid unsigned long (digits only)
- **Whitespace Handling**: Allows optional `\r` after number
- **Duplicate Detection**: Second Content-Length header returns error
- **Zero Valid**: `Content-Length: 0` is valid (no body)

#### GET Request Body Restriction
- **GET Cannot Have Body**: Returns `BODY_NOT_ALLOWED` if GET has `Content-Length > 0`
- **Method Check**: Only checks first character ('G') for efficiency
- **Rationale**: Prevents ambiguous requests per HTTP specification

**Invariant**: `isContentLengthSeen` flag prevents duplicate Content-Length headers.

### Header Parsing

#### Syntax Requirements
- **Colon Required**: Returns `BAD_HEADER_SYNTAX` if no colon found
- **Key Format**: Everything before first colon
- **Value Format**: Everything after first colon and leading whitespace
- **Line Ending**: Must be `\r\n` (CRLF)

#### Whitespace Handling
- **Leading Spaces Trimmed**: Value has leading spaces removed
- **Trailing Spaces Preserved**: Value keeps trailing spaces
- **Key Whitespace**: Not trimmed (included as-is)

**Example:**
```
Content-Type:   text/plain   \r\n
```
- Key: `"Content-Type"`
- Value: `"text/plain   "` (trailing spaces preserved)

#### Special Header Matching
- **Case-Insensitive**: Uses `strncasecmp()` for Content-Length, Content-Type, Connection
- **Exact Length Match**: Compares both string and length
- **Stored As-Is**: Original casing preserved in header array

## Critical Invariants

### Buffer Assumptions
1. **Null-Terminated**: Buffer must have null terminator at end (for string functions)
2. **Sufficient Size**: Buffer must contain complete request line + headers
3. **No Embedded Nulls**: Headers cannot contain null bytes (would truncate parsing)
4. **CRLF Line Endings**: Only `\r\n` supported (not bare `\n`)

### Parsing Invariants
1. **Sequential Parsing**: Request line must precede headers
2. **Empty Line Terminates**: Headers end at first `\r\n\r\n`
3. **No Header Continuations**: Folded headers (obs-fold) not supported
4. **Single Content-Length**: Duplicate Content-Length forbidden

### State Invariants
1. **Clean Initialization**: `initializeHttpInfo()` must be called before each parse
2. **Embedded Arrays**: Headers and path buffers are part of `httpInfo_t` structure (v0.5)
3. **No Partial Parse State**: Parser is stateless (no context between calls)
4. **Atomic Failure**: Parse errors leave `httpInfo_t` in unspecified state (must reinitialize)
5. **Connection Integration**: Parser works with `connection_t` structures in event-driven model (v0.5)

## Known Limitations & Missing Features

### Protocol Features Not Implemented

#### Chunked Transfer Encoding
- **Status**: Error code defined (`UNSUPPORTED_TRANSFER_ENCODING`) but never returned
- **Impact**: Cannot handle requests with `Transfer-Encoding: chunked`
- **Workaround**: None - requests rejected or incorrectly parsed

#### Transfer-Encoding Header
- **Status**: Not checked during parsing
- **Impact**: Chunked requests pass parsing but body parsing fails
- **Future**: Need to detect Transfer-Encoding and return error

#### Host Header Validation
- **Status**: Not required or validated
- **Impact**: Violates HTTP/1.1 specification (Host is mandatory)
- **Workaround**: None - non-compliant requests accepted

#### Request-Line Validation
- **Method Format**: Not validated (accepts any string before space)
- **URI Format**: Not validated (no illegal character checks)
- **URI Length**: No maximum length enforced
- **Path Encoding**: Percent-encoding not decoded

#### Header Count Limit
- **Status**: `TOO_MANY_HEADERS` error defined but never returned
- **Impact**: Can overflow header array (embedded `headers[100]` in `httpInfo_t`, v0.5)
- **Risk**: Buffer overflow vulnerability if more than 100 headers parsed

#### Header Size Limit
- **Status**: `HEADER_TOO_LARGE` error defined but never returned
- **Impact**: No per-header or total header size limit
- **Risk**: Memory exhaustion attack

### Parser Limitations

#### Content-Type Parsing
- **Current**: Stored as opaque buffer view
- **Missing**: No media type parsing (e.g., "text/plain; charset=utf-8")
- **Missing**: No boundary extraction for multipart
- **Missing**: No charset detection

#### Multipart Parsing
- **Status**: Not implemented
- **Impact**: Cannot parse multipart/form-data or multipart/mixed
- **Workaround**: Body returned as opaque blob

#### URL Decoding
- **Status**: Implemented in v0.4 via `decodeUrl()`
- **Feature**: Percent-encoded characters decoded (e.g., `%20` → space)
- **Integration**: Automatic during request parsing

#### Query String Parsing
- **Status**: Path includes query as-is
- **Impact**: No parameter extraction (e.g., `?key=value`)
- **Workaround**: Handler must parse query manually

#### Header Value Parsing
- **Quality Values**: No parsing of `q=` parameters (e.g., Accept headers)
- **List Values**: No splitting of comma-separated values
- **Quoted Strings**: No special handling of quoted values
- **Comments**: RFC 7230 comments not supported

### Edge Cases Not Handled

#### Duplicate Headers
- **Content-Length**: Detected and rejected ✓
- **Other Headers**: Accepted (last value depends on array order)
- **Expected**: Most duplicates should be rejected or merged

#### Invalid Content-Length
- **Non-numeric**: Returns `INVALID_CONTENT_LENGTH` ✓
- **Negative**: Not checked (unsigned long wraps)
- **Overflow**: Not checked (large values may wrap)

#### Malformed Requests
- **Missing Version**: Returns `BAD_REQUEST_LINE` ✓
- **Extra Spaces**: Not handled (may break parsing)
- **Tabs as Whitespace**: Not recognized (only space checked)
- **Missing CRLF**: May cause infinite loop or wrong parsing

#### Binary Safety
- **Body**: Fully binary safe (uses length, not null-termination) ✓
- **Headers**: Assumes no null bytes (would truncate)
- **Path**: Assumes no null bytes

### Performance Limitations

#### Linear Searches
- **strstr()**: Scans entire buffer for each header
- **No Hash Table**: Special headers matched via strcmp for each header
- **No Trie**: Method matching uses full strcmp

#### Redundant Work
- **Content-Length**: Parsed twice (once for extraction, once for storage)
- **Connection Header**: Scanned for "close" after storing full value

## Future Enhancements

### Protocol Compliance
1. **Host Header Validation**: Require Host header for HTTP/1.1
2. **Transfer-Encoding**: Detect and reject chunked encoding (or implement support)
3. **Header Limits**: Enforce max header count and size
4. **Request-Line Validation**: Validate method, URI, version formats

### Parsing Capabilities
1. **Chunked Encoding**: Full chunked transfer encoding support
2. **Multipart**: Parse multipart/form-data and extract boundaries
3. **URL Decoding**: Decode percent-encoded characters in path
4. **Query Parsing**: Extract and parse query parameters
5. **Content-Type**: Parse media type, charset, boundary parameters

### Edge Case Handling
1. **Duplicate Header Policy**: Define merge or reject behavior
2. **Whitespace Normalization**: Handle tabs, extra spaces
3. **Case Normalization**: Lowercase method/header names
4. **Error Recovery**: Partial parse with error details

### Performance Optimizations
1. **Hash Table**: O(1) special header lookup
2. **Method Trie**: Fast method matching
3. **SIMD**: Vectorized CRLF search
4. **Incremental Parsing**: Support partial buffer parsing

### Security Hardening
1. **Size Limits**: Enforce all limits (header count, size, URI length)
2. **Integer Overflow**: Check Content-Length parsing
3. **Buffer Overflow**: Validate header array bounds
4. **Input Validation**: Reject illegal characters in headers/URI
