# connection.c Documentation

## Overview

`connection.c` implements the per-connection state machine for the event-driven HTTP server (v0.5). Each connection is represented by a `connection_t` structure that tracks the complete lifecycle from initial read through response transmission, supporting keep-alive for multiple requests per connection.

## Purpose

The connection module decouples the event loop (server.c) from the HTTP protocol details (httpParser.c, handlers.c). It manages:

1. **State Machine**: Tracks where each connection is in the request-response cycle
2. **Buffer Management**: Allocates and manages per-connection read/write buffers
3. **Incremental I/O**: Handles partial reads/writes across multiple epoll events
4. **Request Pipelining**: Supports multiple pipelined requests in the read buffer
5. **Keep-Alive**: Resets state for next request instead of closing
6. **File Transmission**: Coordinates sendfile() for static file responses

## Data Structures

### Connection State (`conn_state_t`)

```c
typedef enum {
    READING_HEADERS,     // Reading HTTP headers until \r\n\r\n
    READING_BODY,        // Reading request body (if Content-Length > 0)
    PROCESSING,          // Parsing URL, routing, generating response
    WRITING_RESPONSE,    // Sending HTTP response headers/body
    SENDING_FILE,        // Using sendfile() for static files
    CLOSING              // Connection done, ready for cleanup
} conn_state_t;
```

**State Flow:**
```
READING_HEADERS → READING_BODY → PROCESSING → WRITING_RESPONSE → SENDING_FILE
                       ↓              ↓               ↓                ↓
                   PROCESSING     WRITING      SENDING_FILE      READING_HEADERS
                                                                  (keep-alive)
                       ↓              ↓               ↓                ↓
                    CLOSING        CLOSING         CLOSING          CLOSING
```

### Connection Structure (`connection_t`)

```c
typedef struct connection {
    int fd;                    // Client socket file descriptor
    conn_state_t state;        // Current state in lifecycle
    
    // Read buffer (persistent across requests)
    char* read_buf;            // Dynamic buffer for incoming data
    size_t read_len;           // Bytes currently in buffer
    size_t read_cap;           // Buffer capacity
    
    // Parse tracking
    size_t parse_offset;       // How much has been parsed
    size_t header_end;         // Offset of \r\n\r\n in buffer
    size_t body_expected;      // Content-Length value
    size_t body_received;      // Body bytes read so far
    
    // Write buffer (lazy allocated)
    char* write_buf;           // Response headers and body
    size_t write_len;          // Total bytes to write
    size_t write_sent;         // Bytes written so far
    
    // File sending (for static files)
    int root_fd;               // Document root fd (never closed)
    int file_fd;               // Opened file fd (per request)
    off_t file_offset;         // sendfile() offset
    size_t file_remaining;     // Bytes left to send
    
    // Connection management
    int shouldClose;           // Close after response (no keep-alive)
    
    httpInfo_t request;        // Parsed request data
} connection_t;
```

## Core Functions

### `initializeConnection()`

```c
void initializeConnection(connection_t* conn, int fd, int fileRootFd);
```

**Purpose**: Initialize a new connection structure after accept().

**Implementation:**
```c
conn->fd = fd;
conn->state = READING_HEADERS;
conn->header_end = 0;
conn->parse_offset = 0;
conn->read_len = 0;
conn->read_cap = READ_BUFFER_SIZE;  // 4KB initial
conn->read_buf = malloc(READ_BUFFER_SIZE);

conn->write_len = 0;
conn->write_sent = 0;
conn->write_buf = malloc(MIN_RESPONSE_BUFFER);  // 64KB

conn->root_fd = fileRootFd;
conn->file_fd = -1;
conn->file_offset = 0;
conn->file_remaining = 0;
conn->shouldClose = 0;
```

**Key Points:**
- Allocates read buffer (4KB) and write buffer (64KB)
- Initial state is READING_HEADERS
- parse_offset and read_len both start at 0
- root_fd stored for static file serving

### `closeConnection()`

```c
void closeConnection(connection_t* conn);
```

**Purpose**: Clean up connection resources before freeing.

**Implementation:**
```c
close(conn->fd);                     // Close client socket
if (conn->file_fd != conn->root_fd)  // Close file if opened
    close(conn->file_fd);
free(conn->read_buf);                // Free buffers
free(conn->write_buf);
free(conn);                          // Free structure
```

**Lifecycle:**
- Called when state becomes CLOSING
- Or when connection has error/timeout
- Frees all allocated memory

### `connectionHandler()`

```c
uint32_t connectionHandler(connection_t* conn, uint32_t events);
```

**Purpose**: Main entry point from event loop. Processes connection based on current state and available events.

**Return Values:**
- `EPOLLIN`: Wait for socket readable
- `EPOLLOUT`: Wait for socket writable
- `EPOLLIN | EPOLLOUT`: Wait for both
- `UINT32_MAX`: Close connection

**Implementation:**
```c
if (events & EPOLLIN) {
    handleRead(conn);
}

if (conn->state == READING_HEADERS) {
    handleHeaders(conn);
}

if (conn->state == READING_BODY) {
    handleBody(conn);
}

if (conn->state == PROCESSING) {
    handleRequestProcessing(conn);
}

if (conn->state == WRITING_RESPONSE && (events & EPOLLOUT)) {
    handleWrite(conn);
}

if (conn->state == SENDING_FILE && (events & EPOLLOUT)) {
    handleFileSend(conn);
}

if (conn->state == CLOSING) {
    return UINT32_MAX;  // Signal to close
}

// Return appropriate event mask
if (conn->state == WRITING_RESPONSE || conn->state == SENDING_FILE)
    return EPOLLOUT;
else
    return EPOLLIN;
```

## State Handlers

### `handleRead()`

**Purpose**: Read data from socket into read buffer.

```c
void handleRead(connection_t* conn) {
    while (1) {
        ssize_t valread = read(conn->fd, 
                              conn->read_buf + conn->read_len,
                              conn->read_cap - conn->read_len);
        
        if (valread > 0) {
            conn->read_len += valread;
            
            // Buffer full? Reallocate
            if (conn->read_len == conn->read_cap) {
                conn->read_cap *= 2;
                conn->read_buf = realloc(conn->read_buf, conn->read_cap);
            }
        } else if (valread == 0) {
            // Client closed connection
            conn->state = CLOSING;
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No more data available
                return;
            }
            // Error
            conn->state = CLOSING;
            return;
        }
    }
}
```

**Key Points:**
- Loops until EAGAIN (drains socket)
- Appends to existing buffer (supports pipelining)
- Doubles buffer size when full
- Detects connection close (valread == 0)

### `handleHeaders()`

**Purpose**: Search for "\r\n\r\n" to detect end of headers.

```c
void handleHeaders(connection_t* conn) {
    // Search for header end starting from parse_offset
    char* header_end = NULL;
    for (size_t i = conn->parse_offset; i + 3 < conn->read_len; i++) {
        if (conn->read_buf[i] == '\r' &&
            conn->read_buf[i + 1] == '\n' &&
            conn->read_buf[i + 2] == '\r' &&
            conn->read_buf[i + 3] == '\n') {
            header_end = conn->read_buf + i;
            break;
        }
    }
    
    if (!header_end) {
        return;  // Wait for more data (EPOLLIN)
    }
    
    conn->header_end = header_end - conn->read_buf;
    
    // Check header size limit
    size_t header_size = conn->header_end - conn->parse_offset;
    if (header_size > MAX_HEADER_SIZE) {  // 8KB
        handleParseError(HEADER_TOO_LARGE, conn);
        return;
    }
    
    // Parse request line and headers
    parserResult_t res = requestAndHeaderParser(
        conn->read_buf + conn->parse_offset,
        conn->read_buf + conn->header_end + 2,
        &conn->request
    );
    
    if (res != OK) {
        handleParseError(res, conn);
        return;
    }
    
    // Success - move to next state
    conn->state = READING_BODY;
    conn->parse_offset += header_size + 4;  // +4 for \r\n\r\n
}
```

**Key Points:**
- Only searches new data (from parse_offset)
- Returns early if headers incomplete
- Enforces 8KB header size limit
- Advances parse_offset to body start

### `handleBody()`

**Purpose**: Read request body if Content-Length > 0.

```c
void handleBody(connection_t* conn) {
    char* bodyStart = conn->read_buf + conn->parse_offset;
    parserResult_t res = bodyParser(bodyStart, &conn->request);
    
    if (res == OK) {
        conn->state = PROCESSING;
    } else {
        conn->state = CLOSING;
    }
}
```

**Key Points:**
- bodyParser() checks if enough bytes available
- If incomplete, returns error and waits for next EPOLLIN
- On success, transitions to PROCESSING

### `handleRequestProcessing()`

**Purpose**: Decode URL, normalize path, route request, generate response.

```c
void handleRequestProcessing(connection_t* conn) {
    // Decode percent-encoding
    parserResult_t res = decodeUrl(&conn->request.path, 
                                   &conn->request.decodedPath);
    if (res != OK) {
        handleParseError(res, conn);
        return;
    }
    
    // Normalize path (remove .., ., etc)
    res = normalizePath(&conn->request.decodedPath,
                       &conn->request.normalizedPath);
    if (res != OK) {
        handleParseError(res, conn);
        return;
    }
    
    // Route request and generate response
    response_t response = requestHandler(&conn->request, conn->root_fd);
    
    // Format response into write buffer
    createWritableResponse(&response, &conn->write_buf, &conn->write_len);
    
    // For static files, save file descriptor
    if (!conn->request.isApi) {
        conn->file_fd = response.fileDescriptor;
        conn->file_remaining = response.fileSize;
        conn->file_offset = 0;
    }
    
    conn->shouldClose = response.shouldClose;
    conn->state = WRITING_RESPONSE;
}
```

**Key Points:**
- URL processing (decode, normalize)
- Calls requestHandler() for routing
- Formats HTTP response headers
- Prepares for file sending if static file
- Transitions to WRITING_RESPONSE

### `handleWrite()`

**Purpose**: Write response headers/body to socket.

```c
void handleWrite(connection_t* conn) {
    while (conn->write_sent < conn->write_len) {
        ssize_t sent = write(conn->fd,
                            conn->write_buf + conn->write_sent,
                            conn->write_len - conn->write_sent);
        
        if (sent > 0) {
            conn->write_sent += sent;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // Wait for EPOLLOUT
            }
            conn->state = CLOSING;
            return;
        }
    }
    
    // Write complete
    if (conn->file_fd != -1) {
        conn->state = SENDING_FILE;
    } else if (conn->shouldClose) {
        conn->state = CLOSING;
    } else {
        // Keep-alive: reset for next request
        resetConnection(conn);
    }
}
```

**Key Points:**
- Loops until EAGAIN or complete
- Tracks write_sent offset
- After headers/body sent:
  - If file_fd set → SENDING_FILE
  - If shouldClose → CLOSING
  - Else → reset for keep-alive

### `handleFileSend()`

**Purpose**: Send static file using sendfile() system call.

```c
void handleFileSend(connection_t* conn) {
    while (conn->file_remaining > 0) {
        ssize_t sent = sendfile(conn->fd, conn->file_fd,
                               &conn->file_offset,
                               conn->file_remaining);
        
        if (sent > 0) {
            conn->file_remaining -= sent;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;  // Wait for EPOLLOUT
            }
            conn->state = CLOSING;
            return;
        }
    }
    
    // File sent completely
    close(conn->file_fd);
    conn->file_fd = -1;
    
    if (conn->shouldClose) {
        conn->state = CLOSING;
    } else {
        resetConnection(conn);
    }
}
```

**Key Points:**
- sendfile() is zero-copy (kernel to socket)
- Updates file_offset and file_remaining
- Closes file_fd when complete
- Supports keep-alive (resetConnection)

### `resetConnection()`

**Purpose**: Prepare connection for next request (keep-alive).

```c
void resetConnection(connection_t* conn) {
    // Shift unparsed data to buffer start
    size_t unparsed = conn->read_len - conn->parse_offset;
    if (unparsed > 0) {
        memmove(conn->read_buf, 
                conn->read_buf + conn->parse_offset,
                unparsed);
    }
    
    conn->read_len = unparsed;
    conn->parse_offset = 0;
    conn->header_end = 0;
    conn->write_sent = 0;
    conn->write_len = 0;
    conn->state = READING_HEADERS;
    
    // If pipelined request already in buffer, process immediately
    if (conn->read_len > 0) {
        handleHeaders(conn);
    }
}
```

**Key Points:**
- Preserves unparsed data (pipelined requests)
- Resets all offsets and lengths
- Returns to READING_HEADERS
- Processes pipelined requests immediately

### `handleParseError()`

**Purpose**: Generate HTTP error response for parse failures.

```c
void handleParseError(parserResult_t res, connection_t* conn) {
    int status = 400;
    char* msg = "Bad Request";
    
    // Map error to status code (error_table lookup)
    switch (res) {
        case BAD_REQUEST_LINE: status = 400; break;
        case INVALID_VERSION: status = 505; break;
        case UNSUPPORTED_METHOD: status = 405; break;
        case HEADER_TOO_LARGE: status = 431; break;
        // ... more mappings
    }
    
    // Format error response
    int len = snprintf(conn->write_buf, conn->write_len,
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        status, msg);
    
    conn->write_len = len;
    conn->write_sent = 0;
    conn->shouldClose = 1;
    conn->state = WRITING_RESPONSE;
}
```

**Key Points:**
- Maps parser errors to HTTP status codes
- Generates minimal error response
- Always closes connection (no keep-alive on errors)

## Buffer Management

### Read Buffer Strategy

**Initial Size**: 4KB (READ_BUFFER_SIZE)
**Growth**: Doubles when full
**Maximum**: 8KB for headers (enforced), unlimited for body

**Why Dynamic?**
- Small initial size saves memory for idle connections
- Grows to accommodate large requests
- Headers limited to prevent DoS attacks

### Write Buffer Strategy

**Initial Size**: 64KB (MIN_RESPONSE_BUFFER)
**Growth**: Reallocated if response larger
**Contents**: HTTP response headers + body (for API routes)

**For Static Files:**
- Write buffer contains only headers
- File body sent via sendfile() (zero-copy)

### Buffer Reuse

**Keep-Alive Connections:**
- Read buffer reused across requests
- Write buffer reused (reset write_sent)
- No reallocation between requests

**Benefits:**
- Reduces malloc/free overhead
- Improves cache locality
- Supports request pipelining

## Request Pipelining

**Definition**: Multiple HTTP requests sent without waiting for responses.

**Example:**
```
GET /page1 HTTP/1.1\r\n\r\n
GET /page2 HTTP/1.1\r\n\r\n
GET /page3 HTTP/1.1\r\n\r\n
```

**Implementation:**

1. **Read Phase**: All pipelined requests read into buffer
2. **Parse Phase**: First request parsed (parse_offset marks end)
3. **Response Phase**: Response sent
4. **Reset Phase**: resetConnection() moves unparsed data to start
5. **Loop**: Immediately process next request if data available

**Key Fields:**
- `read_len`: Total bytes in buffer
- `parse_offset`: Bytes consumed by current request
- `read_len - parse_offset`: Unparsed bytes (pipelined requests)

## Memory Ownership

### Parsed Request Data

**httpInfo_t Fields**:
- All string pointers (method, path, headers) are bufferView_t
- Point directly into read_buf (zero-copy)
- Valid until read_buf modified (realloc/memmove)
- No separate allocations for parsed data

**Lifetime Rules:**
1. Parse data valid after requestAndHeaderParser()
2. Valid through PROCESSING and response generation
3. Invalid after resetConnection() (buffer shifted)
4. For keep-alive, data must be processed before reset

### Dynamic Allocations

**Per Connection:**
- connection_t structure
- read_buf (4KB → grows)
- write_buf (64KB)

**Per Request:**
- decodedPath buffer (inside httpInfo_t)
- normalizedPath buffer (inside httpInfo_t)
- Freed implicitly in resetConnection()

## Error Handling

### Socket Errors

**Read Errors:**
- EAGAIN/EWOULDBLOCK → Normal (wait for EPOLLIN)
- Connection closed (valread == 0) → CLOSING
- Other errors → CLOSING

**Write Errors:**
- EAGAIN/EWOULDBLOCK → Normal (wait for EPOLLOUT)
- EPIPE (broken pipe) → CLOSING
- Other errors → CLOSING

### Parse Errors

Handled by handleParseError():
- Generate HTTP error response
- Set shouldClose = 1
- Transition to WRITING_RESPONSE

### Resource Limits

**Header Size**: 8KB maximum
**Buffer Growth**: Unbounded (TODO: add limit)
**File Size**: OS-limited (sendfile handles large files)

## Performance Considerations

### Zero-Copy Operations

1. **Parsing**: bufferView_t points into read_buf (no copying)
2. **sendfile()**: Kernel sends file directly to socket (no userspace copy)
3. **Buffer Reuse**: Same buffers across keep-alive requests

### Non-Blocking Benefits

- No threads/processes per connection
- Minimal context switches
- Efficient CPU usage
- Scales to 10,000+ connections

### Memory Footprint

**Per Connection (Idle):**
- connection_t: ~200 bytes
- read_buf: 4KB
- write_buf: 64KB
- Total: ~68KB per connection

**10,000 Connections:**
- 680MB RAM (manageable on modern servers)

## Debugging Tips

### Print State Transitions

```c
printf("Connection %d: %s -> %s\n", 
       conn->fd, 
       state_name(old_state), 
       state_name(conn->state));
```

### Track Buffer Usage

```c
printf("Connection %d: read=%zu/%zu, parse=%zu, write=%zu/%zu\n",
       conn->fd, conn->read_len, conn->read_cap,
       conn->parse_offset, conn->write_sent, conn->write_len);
```

### Detect Leaks

Use valgrind to find memory leaks:
```bash
valgrind --leak-check=full ./server
```

## Related Documentation

- [server.md](server.md) - Event loop and epoll details
- [httpParser.md](httpParser.md) - Request parsing implementation
- [handlers.md](handlers.md) - Request routing and responses

## Version History

- **v0.5**: Initial implementation for event-driven architecture
