# httpParser.c Documentation

## Overview

`httpParser.c` implements HTTP/1.1 request parsing with a zero-copy design. It extracts request lines, headers, and body boundaries from raw TCP buffers without allocating memory, returning non-owning views (`bufferView_t`) into the caller's buffer.

## Design Philosophy

### Zero-Copy Parsing
- Parser **never allocates memory**
- Returns `bufferView_t` structs pointing into caller's buffer
- Caller retains buffer ownership and lifetime management
- No string duplication or copying during parse

### Memory Safety Contract
1. **Parser does not own buffer**: Caller must ensure buffer remains valid
2. **Parsed views valid until buffer modification**: Any buffer realloc/memmove invalidates all `bufferView_t` pointers
3. **No null-termination guarantee**: Buffer views may point to middle of buffer (not null-terminated substrings)
4. **Read-only**: Parser never modifies input buffer

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
Complete parsed HTTP request:
```c
typedef struct {
    bufferView_t method;
    bufferView_t path;
    bufferView_t version;
    header_t* headers;           // Array of headers
    int headerCount;
    unsigned long contentLength; // 0 if no Content-Length header
    bufferView_t contentType;    // Empty if no Content-Type header
    bufferView_t body;           // Empty if no body
    int isKeepAlive;             // 1 = keep-alive, 0 = close
    int isContentLengthSeen;     // Internal flag for duplicate detection
} httpInfo_t;
```

## Key Functions

### `initializeHttpInfo(httpInfo_t* httpInfo, header_t* headerArray)`
Initializes `httpInfo_t` with default values before parsing.

**Defaults:**
- All buffer views set to `{NULL, 0}`
- `isKeepAlive = 1` (HTTP/1.1 default)
- `contentLength = 0`
- `isContentLengthSeen = 0`
- `headers` points to caller-provided array

**Invariant**: Must be called before every `requestAndHeaderParser()` call to ensure clean state.

### `requestAndHeaderParser()`
Parses HTTP request line and all headers from buffer.

**Parsing Steps:**
1. **Request Line**: Extract method, path, version (space-delimited)
2. **Version Validation**: Ensure exactly "HTTP/1.1"
3. **Header Loop**: Parse headers until `\r\n\r\n` (empty line)
4. **Special Headers**: Extract Content-Length, Content-Type, Connection
5. **Body Validation**: Verify GET requests have no body

**Returns**: `parserResult_t` (OK on success, error code on failure)

### `bodyParser()`
Creates a buffer view for the request body based on `Content-Length`.

**Behavior:**
- Returns view starting at current buffer position
- View length equals `contentLength` from headers
- Does **NOT** validate that buffer contains full body
- Caller must ensure sufficient bytes available

**Invariant**: Only call after successful `requestAndHeaderParser()` and when `contentLength > 0`.

### Utility Functions

#### `strstr_len()`
Length-bounded `strstr()` - searches for substring within first `n` bytes.

#### `strcasestr_len()`
Case-insensitive length-bounded substring search.

**Use Case**: Search for "close" in Connection header value without null-terminated strings.

## HTTP/1.1 Protocol Requirements

### Version Enforcement
- **Only HTTP/1.1 Accepted**: Returns `INVALID_VERSION` for other versions
- **Exact Match Required**: "HTTP/1.1" (case-sensitive)
- **No HTTP/1.0**: Not supported (different keep-alive semantics)
- **No HTTP/2**: Not supported

**Rationale**: Simplifies keep-alive logic - HTTP/1.1 defaults to persistent connections.

### Keep-Alive Semantics (v0.2 Feature)

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
2. **Header Array Ownership**: Caller provides and owns header array
3. **No Partial Parse State**: Parser is stateless (no context between calls)
4. **Atomic Failure**: Parse errors leave `httpInfo_t` in unspecified state (must reinitialize)

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
- **Impact**: Can overflow header array (hardcoded to 100 in server.c)
- **Risk**: Buffer overflow vulnerability

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
- **Status**: Not implemented
- **Impact**: Percent-encoded characters (e.g., `%20`) not decoded
- **Workaround**: Handler must decode manually

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
