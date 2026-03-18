# Changelog

## [v0.5] - 2026-03-18

### Added
- **Event-Driven Architecture**: Complete rewrite from process-based to epoll-based I/O multiplexing
  - Single-process event loop using Linux `epoll()` for efficient I/O monitoring
  - Non-blocking sockets (`O_NONBLOCK`) for server and all client connections
  - Per-connection state machine tracking request-response lifecycle
  - C10K capable: Handles 10,000+ concurrent connections efficiently
  - Eliminates fork() overhead (no process creation per connection)
  - Memory-efficient: ~68KB per idle connection (vs. full process in v0.4)
- **Connection Module**: New `connection.c/h` implementing state machine
  - `connection_t` structure tracks all per-connection state
  - Six states: `READING_HEADERS`, `READING_BODY`, `PROCESSING`, `WRITING_RESPONSE`, `SENDING_FILE`, `CLOSING`
  - `connectionHandler()`: Main event dispatcher for connection state transitions
  - `initializeConnection()` / `closeConnection()`: Lifecycle management
  - State handlers: `handleRead()`, `handleHeaders()`, `handleBody()`, `handleRequestProcessing()`, `handleWrite()`, `handleFileSend()`
  - `resetConnection()`: Keep-alive support (reset state for next request)
  - Incremental I/O: Handles partial reads/writes across multiple epoll events
- **Non-Blocking I/O Patterns**: Proper EAGAIN/EWOULDBLOCK handling
  - Read loops drain socket until EAGAIN
  - Write loops handle partial sends, return EPOLLOUT on buffer full
  - sendfile() for static files works with non-blocking semantics
- **Epoll Event Loop**: Efficient event multiplexing in `server.c`
  - `epoll_create()`: Initialize epoll instance at startup
  - Server socket registered with EPOLLIN for accept events
  - Non-blocking accept() loop drains all pending connections
  - Client sockets registered with dynamic interests (EPOLLIN/EPOLLOUT)
  - `EPOLL_CTL_MOD` updates interest based on state transitions
  - `EPOLL_CTL_DEL` removes connection when state is CLOSING

### Changed
- **Server Architecture**: Simplified from fork-based to single-process event loop
  - Removed SIGCHLD handler and zombie process reaping (no child processes)
  - Removed socket timeout configuration (replaced with non-blocking I/O)
  - Server socket set to non-blocking mode
  - Listen backlog increased from 3 to 50 connections
- **Handler Interface**: Separated response generation from transmission
  - Removed `sendResponse()`, `sendHeaders()`, `sendBody()`, `sendFileStream()` functions
  - New `createWritableResponse()`: Formats complete response into buffer
  - New `generateResponseHeaders()`: Returns header length (doesn't send)
  - New `addBody()`: Appends body to buffer after headers
  - Handlers are now synchronous (no socket I/O, no blocking)
  - Connection module handles all write() and sendfile() operations
- **Parser Integration**: Moved error handling to httpParser.c
  - `handleParseError()` moved from server.c to httpParser.c
  - Takes `connection_t*` parameter instead of socket fd
  - Formats error responses into `connection->write_buf` (no direct I/O)
  - Sets connection state to `WRITING_RESPONSE` for error transmission
- **Memory Model**: Embedded buffers in httpInfo_t
  - `decodedPathBuf[8192]` and `normalizedPathBuf[8192]` embedded in structure
  - Headers array `headers[100]` embedded (no separate allocation)
  - Path buffers initialized in `initializeHttpInfo()` to point to embedded arrays
  - Reduces heap allocations and improves cache locality

### Improved
- **Performance**: Significant throughput and latency improvements
  - 38% higher request rate vs v0.4 (14,300 req/sec vs 10,300 req/sec)
  - 30% lower mean latency (0.7ms vs 1.0ms)
  - Single-process architecture eliminates fork() and context switch overhead
  - Minimal idle memory per connection (~68KB vs full process)
- **Scalability**: C10K problem solved
  - v0.4: Limited by OS process limits (~10K-100K processes max)
  - v0.5: Handles 10,000+ concurrent connections with single process
  - No thread/process creation per connection
  - Efficient epoll scaling (O(1) event notification)
- **Request Pipelining**: Better support for pipelined requests
  - Read buffer preserves unparsed data across keep-alive resets
  - `resetConnection()` immediately processes pipelined requests if in buffer
  - No blocking delays between pipelined requests

### Removed
- **Process-Based Concurrency**: fork() model replaced with event-driven
  - No more parent-child process model
  - No SIGCHLD handler or zombie process management
  - No copy-on-write semantics or process isolation
- **Blocking I/O**: All I/O now non-blocking
  - Removed socket read timeout (SO_RCVTIMEO)
  - Removed blocking send() loops from handlers
  - Handlers no longer touch sockets directly

### Technical Details
- **Buffer Management**: Dynamic per-connection buffers
  - Read buffer: 4KB initial, grows as needed, 8KB max for headers
  - Write buffer: 64KB for response headers/body
  - Buffers reused across keep-alive requests
- **File Descriptor Management**:
  - Document root fd opened once at startup, shared across all connections
  - Per-request file fd opened in PROCESSING, closed after SENDING_FILE
  - Connection structure tracks both root_fd and file_fd
- **Keep-Alive Implementation**: State reset instead of connection close
  - After response complete, `resetConnection()` called if !shouldClose
  - Shifts unparsed data to buffer start with memmove()
  - Resets state to READING_HEADERS and continues
  - Closes connection only when shouldClose flag set or error occurs

### Documentation
- **New Files**:
  - `docs/connection.md`: Comprehensive connection state machine documentation
- **Updated Files**:
  - `README.md`: Event-driven architecture description and v0.5 features
  - `docs/server.md`: Complete rewrite for epoll event loop model
  - `docs/handlers.md`: Updated for buffer-based interface
  - `docs/httpParser.md`: Integration with connection state machine

### Known Limitations
- **Linux-Only**: Uses epoll (not portable to BSD/macOS without kqueue port)
- **Single-Threaded**: All I/O handled in one thread (CPU-bound operations block event loop)
- **No Connection Limits**: Unlimited concurrent connections (limited by OS fd limits)
- **No Idle Timeouts**: Connections persist indefinitely (TODO: add timeout mechanism)

## [v0.4] - 2026-02-12

### Added
- **Static File Serving**: Server can now serve files from the `public/` directory
  - Uses `openat()` with document root descriptor for secure path resolution
  - Zero-copy file transmission with Linux `sendfile()` syscall
  - Automatic `index.html` serving for root/empty path requests (`GET /` → `public/index.html`)
  - File metadata retrieved with `fstat()` for Content-Length header
- **Content-Type Detection**: Automatic MIME type mapping based on file extensions
  - `.html` → `text/html`
  - `.css` → `text/css`
  - `.js` → `application/javascript`
  - `.png` → `image/png`
  - Default → `application/octet-stream`
  - Content-Type header included in all responses
- **URL Decoding & Path Normalization**: Security-focused path processing
  - `decodeUrl()`: Percent-encoding decoding (e.g., `%20` → space, `%2F` → `/`)
  - `normalizePath()`: Resolves `.` and `..` segments, prevents directory traversal attacks
  - Path validation rejects attempts to escape document root (`../../etc/passwd` blocked)
  - Invalid percent-encoding or malicious paths return 400 Bad Request
- **API vs Static File Routing**: Intelligent request routing
  - Requests with `/api/` prefix handled by API handlers
  - `/api/` prefix stripped from path before routing (`/api/echo` → `/echo`)
  - All other requests serve static files from `public/` directory
  - New `isApi` flag in `httpInfo_t` tracks routing decision
- **HTTP/1.0 Support**: Compatibility with older HTTP clients
  - Parser accepts both `HTTP/1.1` and `HTTP/1.0` version strings
  - HTTP/1.0 requests automatically disable keep-alive (close after response)
  - Enables Apache Bench (`ab`) compatibility
- **Enhanced Error Responses**: More descriptive error handling
  - 404 Not Found: Includes "Route Not Found" body (15 bytes) for missing files/routes
  - 403 Forbidden: Returns when attempting directory access or on permission errors (EACCES)
  - 500 Internal Server Error: Generic fallback for unexpected errors
  - Error code differentiation: ENOENT→404, EACCES→403, others→500
- **Request Timeout Protection**: Prevents slow client attacks
  - 10-second socket read timeout configured via `setsockopt(SO_RCVTIMEO)`
  - Returns 408 Request Timeout when client doesn't send data within limit
  - Per-socket timeout (each child process has independent timeout)

### Changed
- **Response Structure Enhanced**: `response_t` now supports file descriptors and metadata
  - New fields: `fileDescriptor` (int), `fileSize` (size_t), `contentType` (char*)
  - Enables separate handling for malloc'd bodies vs. file-based responses
  - Content-Type dynamically set based on file extension or route type
- **Separate Response Transmission Paths**: Optimized send logic
  - `sendHeaders()`: Sends HTTP status line and headers (Content-Length, Content-Type, Connection)
  - `sendBody()`: Sends malloc'd response body (for API responses)
  - `sendFileStream()`: Uses `sendfile()` for zero-copy file transmission
  - Partial write handling with retry loops for large transfers
- **httpInfo_t Structure Expansion**: Parser tracks additional metadata
  - New fields: `decodedPath` (bufferView_t), `normalizedPath` (char*), `isApi` (int)
  - Parsed data includes both raw path and processed safe path
  - API detection result stored for handler routing
- **Server Initialization**: Opens document root at startup
  - `docroot_fd` opened once on server start (`open("public/", O_RDONLY | O_DIRECTORY)`)
  - Descriptor passed to all child processes via `fork()` inheritance
  - Used with `openat()` for secure relative path resolution
  - Prevents TOCTOU (time-of-check-time-of-use) vulnerabilities

### Performance Characteristics
- **Zero-Copy File Serving**: `sendfile()` eliminates user-space buffer allocation
  - File data transferred directly from kernel page cache to socket buffer
  - ~10,000 req/sec for small static files (measured with Apache Bench)
  - Minimal CPU overhead compared to `read()` + `write()` approach
- **Path Processing Overhead**: URL decoding and normalization add minimal latency
  - Both operations are O(n) where n = path length (typically <100 bytes)
  - Memory allocated for decoded/normalized paths freed after request completion
- **Concurrency Model**: Unchanged from v0.3 (process-per-connection)
  - Each child process serves static files or API responses independently
  - No shared state between processes (no cache invalidation needed)

### Security Enhancements
- **Path Traversal Prevention**: Multiple layers of defense
  - `openat()` with base directory descriptor limits filesystem access
  - `normalizePath()` resolves `..` and rejects paths escaping root
  - Directory access blocked (only regular files served)
- **Input Validation**: Robust percent-encoding handling
  - Validates hex digits in `%XX` sequences
  - Rejects malformed encodings (e.g., `%ZZ`, `%2`, `%`)
  - Null byte injection prevented

### Breaking Changes
- **Route Behavior Changed**: `GET /` now serves `public/index.html` instead of "Hello" message
  - Previous "Hello" response moved to `GET /api/` endpoint
  - All non-API routes attempt static file serving
- **404 Responses Include Body**: Previously sent empty 404s, now includes "Route Not Found" text
  - Content-Length header reflects 15-byte body
  - May affect clients parsing Content-Length

### Documentation
- **Performance Benchmarks**: New [docs/BENCHMARKS.md](docs/BENCHMARKS.md) with Apache Bench results
  - ~10,300 req/sec for static HTML files (820 bytes, c=100)
  - ~11,300 req/sec for API endpoints (c=50)
  - Latency analysis and concurrency scaling tests
- **API Changes Documented**: All component docs updated for v0.4 features

---

## [v0.3] - 2026-02-06

### Added
- **Process-Based Concurrency**: Server now uses `fork()` to handle multiple client connections concurrently
  - Parent process accepts incoming connections in a loop
  - Each connection spawns a child process via `fork()` to handle that specific client
  - Clear separation of concerns: parent accepts, children serve
- **Zombie Process Reaper**: Implemented `SIGCHLD` signal handler with `burnZombies()` cleanup
  - Prevents accumulation of defunct child processes
  - Uses `waitpid(-1, NULL, WNOHANG)` for non-blocking reaping
  - Signal handler preserves `errno` for signal-safe operation
- **Concurrent Client Support**: Server no longer limited to single connection; can serve multiple clients simultaneously

### Changed
- **Server Loop Architecture**: Main loop now forks instead of blocking on client operations
  - Parent loop: Socket accept → Fork child → Close child socket descriptor → Loop
  - Child process: Close server socket → Handle client completely → Exit

### Performance Characteristics
- **Concurrency Model**: Process-per-connection (scales to OS limits)
- **Memory Overhead**: Each child process is a full fork (cow—copy-on-write for memory efficiency)
- **No Timeout Issues**: Multiple concurrent clients don't block each other

### Breaking Changes
- None for client API; server behavior is backward-compatible

---

## [v0.2] - Initial Release When documentation began

### Added
- **HTTP/1.1 Protocol Support**: Full request parsing and response generation
- **Persistent Connections (Keep-Alive)**: Clients can issue multiple requests over single TCP connection
  - HTTP/1.1 defaults to keep-alive; respects `Connection: close` header
  - Connection persists until client closes or `Connection: close` is sent
- **Request Pipelining**: Server handles multiple requests buffered in single read
  - Parser supports fragmented requests across socket reads
  - Buffer shifting allows sequential parsing of pipelined requests
- **Zero-Copy Parsing**: Memory-efficient request parsing
  - Parser returns `bufferView_t` views into connection buffer
  - No memory allocation during parsing; parser never calls `malloc()`
  - Eliminates unnecessary string copying and heap overhead
- **Dynamic Buffer Management**: Per-connection buffers grow as needed
  - Initial allocation: 4 KB; Max limit: 16 KB
  - Automatic reallocation with size limit enforcement
  - Returns 413 Payload Too Large when limit exceeded
- **Request Routing**: Two endpoints implemented
  - `GET /` — Returns "Hello" message
  - `POST /echo` — Echoes request body back to client
- **Comprehensive Error Handling**: Maps parse errors to appropriate HTTP status codes
  - 400 Bad Request, 405 Method Not Allowed, 413 Payload Too Large, 500 Internal Server Error, etc.
  - Graceful error responses with proper headers

### Features
- Binary-safe request/response bodies
- Configurable buffer limits (growth strategy)
- HTTP error response generation
- Proper Content-Length handling
- Connection header management

### Limitations
- Single connection at a time (sequential processing)
- No concurrency mechanism
- Server exits after client disconnects
