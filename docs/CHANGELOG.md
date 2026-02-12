# Changelog

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
