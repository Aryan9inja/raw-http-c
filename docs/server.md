# server.c Documentation

## Overview

`server.c` is the main entry point and orchestrator of the HTTP/1.1 server. It manages TCP socket operations, connection lifecycle, buffer management, and coordinates the request-response pipeline through the parser and handler components.

## Key Responsibilities

1. **TCP Socket Setup**: Creates, binds, and listens on port 8080
2. **Connection Acceptance**: Accepts incoming client connections
3. **Buffer Management**: Dynamically allocates and manages per-connection buffers
4. **Request Processing Loop**: Implements keep-alive connection handling with request pipelining
5. **Error Handling**: Maps parser errors to HTTP status codes and generates error responses
6. **Resource Cleanup**: Ensures proper cleanup of memory and socket descriptors

## Core Functions

### `handleParseError()`
Maps parser error codes (`parserResult_t`) to appropriate HTTP status codes and sends error responses to the client.

**Error Mapping:**
- `BAD_REQUEST_LINE`, `BAD_HEADER_SYNTAX`, `INVALID_CONTENT_LENGTH`, etc. → 400 Bad Request
- `UNSUPPORTED_METHOD` → 405 Method Not Allowed
- `HEADER_TOO_LARGE`, `TOO_MANY_HEADERS` → 431 Request Header Fields Too Large
- `PAYLOAD_TOO_LARGE` → 413 Payload Too Large
- `UNSUPPORTED_TRANSFER_ENCODING` → 501 Not Implemented
- Memory allocation failure → 500 Internal Server Error

### `main()`
The main server loop that orchestrates all operations:

1. Socket initialization and binding
2. Connection acceptance loop
3. Per-connection request processing loop with keep-alive support
4. Buffer management with dynamic reallocation
5. Request parsing and handling
6. Response transmission
7. Cleanup and connection termination

## Buffer Management Model

### Initial Allocation
- **Initial Size**: 4096 bytes (`BUFFER_SIZE`)
- **Maximum Size**: 16384 bytes (`MAX_REQUEST_LIMIT`)
- **Null Termination**: Buffer always null-terminated after reads for string safety

### Dynamic Reallocation Strategy
When a request exceeds the current buffer size:
1. Check if doubling the buffer would exceed `MAX_REQUEST_LIMIT`
2. If within limit, reallocate buffer to double size
3. If exceeding limit, return 413 Payload Too Large error
4. If reallocation fails, return 500 Internal Server Error

### Buffer Offsets
- **`readOffset`**: Total bytes read from socket into buffer (cumulative)
- **`parseOffset`**: Bytes successfully consumed by parser (cumulative)
- **Invariant**: `parseOffset ≤ readOffset` at all times

### Buffer Shifting
After successfully parsing a request:
```
unparsedBytes = readOffset - parseOffset
```
If `unparsedBytes > 0` (pipelined requests present):
- Use `memmove()` to shift unparsed bytes to buffer start
- Reset `readOffset = unparsedBytes`
- Reset `parseOffset = 0`
- Continue to parse next request

## Keep-Alive Connection Logic (v0.2 Feature)

### Connection Lifecycle
1. Accept connection
2. Allocate buffer and header array
3. **Inner Loop**: Read → Parse → Handle → Respond
4. After each response, check `response.shouldClose`:
   - If `shouldClose == 1`: Break to cleanup (close connection)
   - If `shouldClose == 0`: Continue loop (keep-alive)
5. Cleanup and close connection

### Keep-Alive Flow
```
┌─────────────────────────────────────┐
│      Accept Connection              │
└──────────────┬──────────────────────┘
               │
               ▼
┌─────────────────────────────────────┐
│   Allocate Buffer & Headers         │
└──────────────┬──────────────────────┘
               │
               │  ╔═══════════════════════════════╗
               └─>║   Keep-Alive Request Loop     ║
                  ╚═══════════════════════════════╝
                               │
                               ▼
                  ┌────────────────────────┐
                  │    Read Request Data   │
                  │  (from socket buffer)  │
                  └───────────┬────────────┘
                              │
                              ▼
                  ┌────────────────────────┐
                  │    Parse Request       │
                  │  (headers & body)      │
                  └───────────┬────────────┘
                              │
                              ▼
                  ┌────────────────────────┐
                  │   Handle Request       │
                  │  (route & generate)    │
                  └───────────┬────────────┘
                              │
                              ▼
                  ┌────────────────────────┐
                  │   Send Response        │
                  │  (headers & body)      │
                  └───────────┬────────────┘
                              │
                              ▼
                  ┌────────────────────────┐
                  │  shouldClose == 1?     │
                  │ (check Connection hdr) │
                  └───┬──────────────┬─────┘
                      │              │
                     NO             YES
                      │              │
                      │              ▼
                      │   ┌─────────────────────┐
                      │   │  Cleanup & Close    │
                      │   │ - Free buffer       │
                      │   │ - Free headers      │
                      │   │ - Close socket      │
          Keep-Alive  |   │ - Exit connection   │
          (loop back) |   └─────────────────────┘
                      │
                      └──────┐
                             │
                             └──> (back to Read Request Data)
```

### Request Pipelining Support
The server can process multiple pipelined requests from a single connection:
- Client may send multiple requests without waiting for responses
- Buffer preserves unparsed bytes after each request
- Inner parse loop processes all complete requests in buffer
- Continues reading only after processing buffered requests

## Process-Based Concurrency (v0.3)

### Multi-Process Architecture
The server uses `fork()` to handle multiple concurrent client connections simultaneously:

**Parent Process Responsibilities:**
1. Create and bind server socket on port 8080
2. Call `listen()` to mark socket as accepting connections
3. **Infinite accept loop**: Call `accept()` to wait for incoming connections
4. When connection arrives, call `fork()` to create child process
5. Parent closes the client socket descriptor (child uses it)
6. Parent returns to step 3 to accept next connection
7. Install `SIGCHLD` signal handler to reap zombies at startup

**Child Process Responsibilities:**
1. Inherit open client socket from parent via `fork()`
2. Close server socket descriptor immediately (only parent listens)
3. Run `handleClient()` to process HTTP requests on that socket
4. Handle keep-alive connections as separate entity from other clients
5. Exit (via `exit(EXIT_SUCCESS)` in `handleClient()`) when client closes connection

### Zombie Process Reaping

**Problem:** When a child process exits, it becomes a "zombie" — its process table entry remains until parent calls `wait()` or `waitpid()`

**Solution: SIGCHLD Handler**
```c
void burnZombies(int s) {
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);  // Reap all available zombies
    errno = saved_errno;
}
```

**Signal Handler Setup:**
```c
struct sigaction sa;
sa.sa_handler = burnZombies;
sigemptyset(&sa.sa_mask);
sa.sa_flags = SA_RESTART;  // Restart interrupted system calls
sigaction(SIGCHLD, &sa, NULL);
```

**How It Works:**
1. Kernel sends `SIGCHLD` signal when child process exits
2. Parent's signal handler runs asynchronously
3. `waitpid(-1, NULL, WNOHANG)` reaps one zombie child
4. Loop continues to reap all available zombies (non-blocking mode)
5. `errno` saved/restored for signal safety (avoid breaking concurrent system calls)

**Key Points:**
- Non-blocking wait (`WNOHANG`): Handler doesn't block parent's main loop
- Loop until no more zombies available (handles burst of child exits)
- `SA_RESTART` flag: Interrupted system calls (like `accept()`) automatically restart
- Signal-safe code: Only `waitpid()` called in handler (other calls could corrupt state)

### Process Lifecycle Example

```
┌──────────────────────────────────────────────────────────────┐
│                    Parent Process                            │
│                                                               │
│  1. socket() → 2. bind() → 3. listen()                       │
│                       ↓                                       │
│  4. Setup SIGCHLD handler (reaps zombies)                   │
│                       ↓                                       │
│  ┌───────────────────────────────────┐                       │
│  │ 5. Infinite Loop:                 │                       │
│  │    accept() → wait for connection │                       │
│  │       ↓ (connection arrives)       │                       │
│  │    fork()                          │                       │
│  │       ├─── Child (PID != 0)        │                       │
│  │       │    → close(server_fd)      │                       │
│  │       │    → handleClient(sock)    │                       │
│  │       │    → exit()                │                       │
│  │       │                            │                       │
│  │       └─── Parent (PID == 0)       │                       │
│  │            → close(client_sock)    │                       │
│  │            → back to accept()      │                       │
│  │                                    │                       │
│  └───────────────────────────────────┘                       │
│                                                               │
│  Note: SIGCHLD handler runs asynchronously                  │
│        when children exit, reaping zombies                  │
└──────────────────────────────────────────────────────────────┘
```

### Concurrency Model Characteristics

**Advantages:**
- **Process Isolation**: Each client completely isolated from others (memory, file descriptors, signals)
- **No Synchronization**: No mutex/lock needs (no shared state between processes)
- **Crash Safety**: One client's misbehavior (segfault, infinite loop) doesn't affect others
- **Simple Cleanup**: Process exit automatically closes client socket and frees memory
- **Inherited Resources**: Child shares program text (read-only), inherits parent state via copy-on-write

**Disadvantages:**
- **Memory Overhead**: Each process duplicates program data / stacks (even with copy-on-write)
- **Process Creation Cost**: `fork()` and context switches more expensive than thread creation
- **Scalability Limit**: OS limits on process count (practical limit: 10K-100K processes)
- **No Shared Resources**: Can't easily pool resources like database connections across clients

**Suitable For:**
- Learning process-based concurrency model
- Moderate connection counts (< 1K concurrent clients)
- Teaching systems concepts: fork, wait, signals, process isolation

## Critical Invariants

### Buffer Invariants
1. **Size Bounds**: `0 < buffer_size ≤ MAX_REQUEST_LIMIT`
2. **Offset Ordering**: `parseOffset ≤ readOffset ≤ buffer_size`
3. **Null Termination**: `buffer[readOffset] == '\0'` after each read
4. **Non-overlapping Shift**: `memmove()` handles overlapping memory correctly

### Connection Invariants
1. **Single Connection**: Server handles exactly one connection at a time
2. **Sequential Processing**: Requests processed in arrival order (no interleaving)
3. **Clean State**: Each request starts with fresh `httpInfo_t` initialization
4. **Deterministic Cleanup**: Connection always closed via cleanup label

### Parser Contract
1. **Buffer Ownership**: Server owns buffer, parser receives non-owning views
2. **Parsed Data Lifetime**: `bufferView_t` pointers valid only until next buffer modification
3. **No Side Effects**: Parser never modifies input buffer
4. **Error Atomicity**: Parse errors leave buffer in consistent state

## Known Limitations & Missing Features

### Concurrency & Scalability (v0.3 Partial Improvement)
- ✅ **Process-Per-Connection**: Server now handles multiple concurrent clients via `fork()`
- ✅ **Zombie Process Cleanup**: SIGCHLD handler prevents accumulation of defunct children
- ❌ **No Threading**: No alternative threaded model yet
- ❌ **No Event Loop**: Blocking I/O only (no epoll/kqueue/select)
- ❌ **Small Listen Backlog**: `listen(3)` provides minimal queue

### Timeouts & Resource Management
- **No Connection Timeout**: Can hang indefinitely waiting for client data
- **No Request Timeout**: Slow clients can hold connection indefinitely
- **No Idle Timeout**: Keep-alive connections never timeout
- **No Read Timeout**: `read()` blocks forever if client stops sending
- **No Max Connections**: No limit on sequential connections

### Configuration & Flexibility
- **Hardcoded Port**: Port 8080 not configurable via arguments or config file
- **Fixed Buffer Sizes**: Constants not adjustable at runtime
- **No Environment Variables**: Cannot configure via ENV
- **No Command-line Arguments**: No flags for port, log level, etc.

### Error Handling & Recovery
- **Fatal Parse Errors**: Any parse error terminates connection (no recovery)
- **No Graceful Degradation**: Cannot downgrade protocol version
- **Memory Allocation Failures**: Only some failures handled (inconsistent)
- **No Error Logging**: Errors not logged to file or stderr
- **Connection Abort Handling**: Abrupt client disconnect may leave inconsistent state

### Observability & Debugging
- **No Logging Framework**: Only basic `printf()` statements
- **No Request Logging**: Method/path/status not recorded
- **No Client IP Logging**: Source address not captured
- **No Metrics**: No connection count, request rate, error rate tracking
- **No Access Logs**: Cannot replay or audit requests
- **No Debug Mode**: No verbose logging option

### Signal Handling & Lifecycle
- ✅ **SIGCHLD Handler (v0.3)**: Reaps child processes via `burnZombies()` function
- ❌ **No Graceful Shutdown**: SIGINT/SIGTERM terminate abruptly
- ❌ **No Other Signal Handlers**: Cannot handle SIGPIPE, SIGHUP, etc.
- ❌ **No Cleanup on Exit**: Resources may leak on abnormal termination
- ❌ **No Daemon Mode**: Cannot run as background service

### Protocol Features
- **No HTTP/1.0 Support**: Only HTTP/1.1 accepted
- **No HTTP/2**: No protocol upgrade support
- **No HTTPS/TLS**: Plain HTTP only
- **No Compression**: gzip/deflate not supported in any layer

### Performance Optimizations
- **No Zero-Copy I/O**: No sendfile() or splice() usage
- **No Buffer Pooling**: Allocates/frees buffer per connection
- **No Header Caching**: Parsed headers not cached across requests
- **Linear Error Lookup**: Error table scanned sequentially

### Security & Hardening
- **No Rate Limiting**: Vulnerable to request flooding
- **No Connection Limits**: No per-IP connection restrictions
- **No Request Size Validation**: Relies only on buffer limit
- **No Input Sanitization**: Minimal validation of paths/headers
- **No DoS Protection**: No slowloris or similar attack mitigation

## Future Enhancements

1. **Multi-threading**: Thread pool for concurrent connections
2. **Timeout System**: Configurable timeouts for all I/O operations
3. **Signal Handling**: Graceful shutdown on SIGTERM/SIGINT
4. **Logging System**: Structured logging with log levels
5. **Configuration**: Command-line arguments and config file support
6. **Metrics & Monitoring**: Request/response statistics
7. **Resource Limits**: Max connections, max requests per connection
8. **Performance**: Buffer pooling, zero-copy I/O, optimized lookups
9. **Protocol Extensions**: HTTP/1.0 support, upgrade mechanisms
10. **Security**: Rate limiting, input validation, DoS protection
