# Changelog

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
