# HTTP/1.1 Server in C - v0.3

A lightweight HTTP/1.1 server implementation in C with process-based concurrency, persistent connection support (keep-alive), request pipelining, and zero-copy parsing.

## Features

### Core Capabilities
- **Process-Based Concurrency (v0.3)**: Handle multiple concurrent clients using `fork()` 
  - Parent process accepts connections; child processes handle individual clients
  - Clear process isolation and lifecycle management
  - Zero-copy architecture maintained across concurrent connections
- **HTTP/1.1 Protocol**: Full HTTP/1.1 request parsing and response generation
- **Keep-Alive Connections**: Persistent connections with multiple requests per connection within each client process
- **Request Pipelining**: Handle multiple pipelined requests efficiently
- **Zero-Copy Parsing**: Memory-efficient parsing using buffer views without duplication
- **Binary Safe**: Handles arbitrary byte sequences in request/response bodies
- **Dynamic Buffers**: Automatic buffer growth up to configurable limits

### Current Endpoints
- `GET /` - Returns "Hello" message
- `POST /echo` - Echoes the request body back to client

## Architecture

The server is organized into three main components with process-based concurrency:

### 0. Server Architecture (v0.3)
- **Parent Process**: Single server loop that listens on port 8080
  - Accepts incoming TCP connections in an infinite loop
  - Forks a child process for each new connection
  - Returns to listening for the next connection
  - Reaps child processes via `SIGCHLD` handler (prevents zombies)
- **Child Process**: One process per client connection
  - Inherits open client socket from parent via `fork()`
  - Closes server socket descriptor (not needed)
  - Runs the complete HTTP connection lifecycle independently
  - Exits after client closes connection

### 1. Server Core ([server.c](server.c))
- TCP socket management and connection lifecycle
- Dynamic buffer allocation and management (4KB initial, 16KB max)
- Request pipelining with read/parse offset tracking
- Keep-alive connection loop
- Error handling and HTTP error response generation

**Documentation**: [docs/server.md](docs/server.md)

### 2. HTTP Parser ([httpParser.c](httpParser.c))
- Zero-copy HTTP/1.1 request parsing
- Request line extraction (method, path, version)
- Header parsing into key-value pairs
- Content-Length and Connection header processing
- Body boundary detection
- Keep-alive detection via Connection header

**Documentation**: [docs/httpParser.md](docs/httpParser.md)

### 3. Request Handlers ([handlers.c](handlers.c))
- Request routing and dispatch
- Response generation with appropriate status codes
- HTTP response formatting and transmission
- Keep-alive state propagation
- Memory management for response bodies

**Documentation**: [docs/handlers.md](docs/handlers.md)

## Design Principles

### Zero-Copy Parsing
The parser uses `bufferView_t` structures (pointer + length) to reference data directly in the connection buffer without copying or allocating memory. This eliminates unnecessary allocations while maintaining safety through careful lifetime management.

### Buffer Model
- **Dynamic Per-Connection Buffer**: Each connection has its own buffer that grows as needed
- **Read/Parse Offsets**: Track bytes read vs. bytes parsed to support pipelining
- **Buffer Shifting**: After parsing, unparsed bytes are moved to buffer start using `memmove()`
- **Incremental Parsing**: Handles fragmented requests across multiple `read()` calls

### Memory Ownership Contract
- **Server owns buffer**: Connection handler allocates, reallocates, and frees the buffer
- **Parser returns views**: All parsed data (method, path, headers, body) are non-owning `bufferView_t` pointers into the buffer
- **Lifetime constraints**: Parsed data is valid only until the next buffer modification (realloc, memmove, or free)
- **No parser allocations**: Parser never calls `malloc()` - zero memory overhead

### Keep-Alive Implementation (v0.2)
HTTP/1.1 connections default to persistent mode:
1. Parser checks for `Connection: close` header
2. Sets `isKeepAlive` flag in parsed request
3. Handler mirrors flag to `shouldClose` in response
4. Response includes appropriate `Connection` header
5. Server loop continues or exits based on `shouldClose`

### Process-Based Concurrency Model (v0.3)
The server uses `fork()` to handle multiple concurrent clients without threading or async I/O:

**Process Lifecycle:**
1. Parent process calls `accept()` to wait for incoming connections
2. When connection arrives, parent calls `fork()` to spawn child process
3. Child process inherits open client socket via copy-on-write semantics
4. Child closes server socket descriptor (only parent listens)
5. Child runs `handleClient()` to process HTTP requests on that connection
6. Child exits when client closes connection
7. Parent continues loop; returns to `accept()` to wait for next connection
8. Zombie processes reaped by `SIGCHLD` handler using `waitpid(-1, NULL, WNOHANG)`

**Concurrency Model:**
- **Process Isolation**: Each client runs in isolated process; memory, file descriptors, and state are separate
- **No Shared State**: Processes don't share buffers or data (no synchronization needed)
- **Copy-on-Write**: Parent and child share memory pages until written (efficient fork)
- **Simple Cleanup**: Child process exit automatically closes client socket and frees memory
- **Scalability Limit**: Limited by OS process limits (typically 10K-100K processes)

**Comparison to v0.2:**
- v0.2: Single process, one client at a time, sequential connections
- v0.3: Multiple processes, many concurrent clients, parallel handling

## Protocol Support

### Supported
- HTTP/1.1 request/response
- Persistent connections (keep-alive)
- Request pipelining
- Content-Length based body parsing
- Binary request/response bodies
- GET and POST methods
- Connection header handling

### Not Yet Supported
- HTTP/1.0 (different keep-alive semantics)
- HTTP/2
- Chunked transfer encoding
- Multipart/form-data parsing
- Static file serving (planned for future release)
- HTTPS/TLS
- Additional HTTP methods (HEAD, PUT, DELETE, OPTIONS, etc.)
- Compression (gzip/deflate)

## Building and Running

### Build
```bash
make          # Build with debug symbols and warnings (dev mode)
make prod     # Build production version
```

### Run
```bash
make run      # Build and start server on port 8080
```

### Clean
```bash
make clean    # Remove build artifacts
```

## Testing

Example using `curl`:

```bash
# GET request
curl http://localhost:8080/

# POST echo
curl -X POST http://localhost:8080/echo -d "Hello, Server!"

# Keep-alive (multiple requests, single connection)
curl -v http://localhost:8080/ http://localhost:8080/echo

# Request with explicit Connection: close
curl -H "Connection: close" http://localhost:8080/
```

## Limitations & Known Issues

### Current Limitations
- **Process Overhead**: Each connection spawns a new process (more memory per client than threads)
- **No Async I/O**: Process blocks on socket reads (not a concern with separate processes)
- **Limited Routes**: Only 2 endpoints (/ and /echo)
- **No Static Files**: Cannot serve HTML, CSS, JS, or other files
- **No Timeouts**: Connections can hang indefinitely (10-second socket read timeout exists)
- **Hardcoded Port**: Always uses port 8080
- **No Logging**: Minimal debug output only
- **No Thread Pool**: Creates new process per connection (suitable for moderate concurrency, not high C10K workloads)

### Fixed in v0.3
- ✅ **Concurrent Connections**: Server now handles multiple clients simultaneously via `fork()`
- ✅ **Zombie Processes**: Prevented with `SIGCHLD` handler reaping children

See individual component documentation for detailed limitations and assumptions.

## Future Goals
### v0.4 — Static File Serving
- Primary concept: filesystem + HTTP interaction
- Serve files from a document root
- Content-Type detection
- Safe path resolution
- Efficient file streaming

### Long-term Goals
- Configurable ports and settings
- Comprehensive logging system
- Chunked transfer encoding
- Additional HTTP methods (HEAD, PUT, DELETE, OPTIONS)
- HTTP compression support
- Performance optimizations (buffer pooling, zero-copy I/O)

## Code Attribution

All of the C code till now is written by **Aryan Singh Thakur** only. The documentation is written with the help of AI.

Each version exists to teach one systems concept. See [docs/CHANGELOG.md](docs/CHANGELOG.md) for detailed release notes and feature history.