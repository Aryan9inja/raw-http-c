# HTTP/1.1 Server in C - v0.5

A lightweight HTTP/1.1 server implementation in C with static file serving, event-driven architecture using epoll, persistent connection support (keep-alive), request pipelining, and zero-copy parsing.

## Deep Dive
1. server.c
   
   <img width="740" height="559" alt="server c" src="https://github.com/user-attachments/assets/ad8d0697-2860-4e07-895b-499b38aea984" />

2. connection.c

   <img width="1268" height="558" alt="connection c" src="https://github.com/user-attachments/assets/44ec1f73-b44c-4b54-b46e-24d28531ee2e" />

3. STATE-MACHINE

   <img width="585" height="735" alt="stateMachine" src="https://github.com/user-attachments/assets/07f9c3fd-065b-425c-b10e-22f48a0cd1c0" />

4. httpParser.c

   <img width="600" height="313" alt="httpParser c" src="https://github.com/user-attachments/assets/3a7e29fc-4327-435f-b729-f93dfc4d44d5" />

5. handlers.c

   <img width="577" height="297" alt="handlers" src="https://github.com/user-attachments/assets/82980927-b02b-406c-b050-74268032e772" />


   

## Features

### Core Capabilities
- **Event-Driven Architecture (v0.5)**: Single-process event loop using epoll for I/O multiplexing
  - Non-blocking sockets with level-triggered epoll notifications
  - Per-connection state machine for lifecycle management
  - Scalable to 10,000+ concurrent connections (C10K capable)
  - Zero process/thread creation overhead
  - Memory-efficient connection handling
- **Static File Serving (v0.4)**: Serve files from `public/` directory with zero-copy `sendfile()`
  - Automatic Content-Type detection based on file extensions
  - Secure path resolution with `openat()` (prevents directory traversal)
  - Default `index.html` serving for root path
- **HTTP/1.1 & HTTP/1.0 Protocol**: Full request parsing and response generation
- **Keep-Alive Connections**: Persistent connections with multiple requests per connection
- **Request Pipelining**: Handle multiple pipelined requests efficiently
- **Zero-Copy Parsing**: Memory-efficient parsing using buffer views without duplication
- **Binary Safe**: Handles arbitrary byte sequences in request/response bodies
- **Dynamic Buffers**: Automatic buffer growth up to configurable limits
- **URL Security**: Percent-encoding decoding and path normalization (v0.4)

### Current Endpoints
- **Static Files**: Any file in `public/` directory (e.g., `GET /`, `GET /style.css`, `GET /script.js`)
- **API Routes** (prefix with `/api/`):
  - `GET /api/` - Returns "Hello" message
  - `POST /api/echo` - Echoes the request body back to client

## Architecture

The server uses an event-driven architecture with epoll for efficient I/O multiplexing:

### 0. Event-Driven Architecture (v0.5)
- **Single-Process Event Loop**: Server runs in a single process with non-blocking I/O
  - Creates epoll instance to monitor multiple socket events
  - Server socket registered with EPOLLIN for accepting connections
  - Each client connection runs as a state machine tracked by epoll
  - No process/thread creation overhead per connection
  
- **Connection State Machine**: Per-connection state tracking
  - `READING_HEADERS`: Reading and parsing HTTP headers
  - `READING_BODY`: Reading request body (if Content-Length > 0)
  - `PROCESSING`: URL decoding, normalization, and request routing
  - `WRITING_RESPONSE`: Sending HTTP response headers/body
  - `SENDING_FILE`: Transmitting file via sendfile() (for static files)
  - `CLOSING`: Cleanup and connection termination

- **Non-Blocking I/O**: All socket operations are non-blocking
  - Server socket set to O_NONBLOCK for accept()
  - Client sockets set to O_NONBLOCK for read()/write()
  - epoll_wait() blocks until events occur (level-triggered)
  - Handles EAGAIN/EWOULDBLOCK for partial I/O operations

- **Event Handling Flow**:
  1. epoll_wait() returns ready file descriptors
  2. Server socket events → accept() new connections in loop
  3. Client socket events → connectionHandler() processes state transitions
  4. Handler returns event mask (EPOLLIN/EPOLLOUT/close)
  5. epoll interest updated via EPOLL_CTL_MOD
  6. Loop continues indefinitely

### 1. Server Core ([server.c](server.c))
- TCP socket management with epoll event loop
- Non-blocking socket configuration
- Accept loop for new connections
- Connection state tracking and event dispatching
- Error handling and cleanup

**Documentation**: [docs/server.md](docs/server.md)

### 2. Connection Handler ([connection.c](connection.c))
- Per-connection state machine implementation
- Dynamic buffer allocation and management (4KB initial read, 64KB write)
- Request parsing coordination across multiple read() calls
- Response generation and transmission
- File descriptor management for static files
- Keep-alive connection handling

**Documentation**: [docs/connection.md](docs/connection.md)

### 3. HTTP Parser ([httpParser.c](httpParser.c))
- Zero-copy HTTP/1.1 request parsing
- Request line extraction (method, path, version)
- Header parsing into key-value pairs
- Content-Length and Connection header processing
- Body boundary detection
- Keep-alive detection via Connection header
- URL decoding and path normalization

**Documentation**: [docs/httpParser.md](docs/httpParser.md)

### 4. Request Handlers ([handlers.c](handlers.c))
- Request routing and dispatch
- Response generation with appropriate status codes
- HTTP response formatting
- Keep-alive state propagation
- Simplified interface for event-driven model

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
5. Connection state machine resets to READING_HEADERS or closes

### Event-Driven Concurrency Model (v0.5)
The server uses epoll for efficient I/O multiplexing without threads or processes:

**Event Loop Architecture:**
1. Single process runs infinite epoll_wait() loop
2. Server socket monitored for EPOLLIN (new connections)
3. Each client socket tracked independently with state machine
4. Non-blocking accept() loop drains all pending connections
5. Each connection allocated as `connection_t` structure
6. Connection pointer stored in epoll_event.data.ptr
7. Events dispatched to connectionHandler() based on state
8. Handler returns event mask for next epoll interest
9. Connection closed and freed when state becomes CLOSING

**Concurrency Model:**
- **Single Process**: All connections handled in one process (no fork/threads)
- **State Machines**: Each connection is an independent state machine
- **Non-Blocking I/O**: All socket operations return immediately with partial results
- **Event Multiplexing**: epoll efficiently monitors 1000s of sockets
- **Memory Efficient**: Per-connection buffers only (no process/thread overhead)
- **Scalability**: C10K capable - 10,000+ concurrent connections

**Comparison to v0.4:**
- v0.4: Process-per-connection, fork() overhead, process isolation
- v0.5: Single process, state machines, epoll-based, memory efficient

## Protocol Support

### Supported
- HTTP/1.1 and HTTP/1.0 request/response
- Persistent connections (keep-alive)
- Request pipelining
- Content-Length based body parsing
- Content-Type header generation
- Binary request/response bodies
- GET and POST methods
- Connection header handling
- Static file serving from document root
- URL percent-encoding decoding

### Not Yet Supported
- HTTP/2
- Chunked transfer encoding
- Multipart/form-data parsing
- HTTPS/TLS
- Additional HTTP methods (HEAD, PUT, DELETE, OPTIONS, etc.)
- Compression (gzip/deflate)
- Range requests (partial content)
- Caching headers (ETag, Last-Modified)

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

## Performance

See [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for detailed Apache Bench results.

**Key Metrics** (AMD Ryzen 5 5600H, 12 cores):
- Static file serving: ~14,300 req/sec (HTML, c=10)
- API endpoints: ~14,300 req/sec (c=10)
- Latency: <0.7ms mean at low concurrency
- Zero-copy `sendfile()` efficiency for static files
- Single-process architecture eliminates fork() overhead

## Testing

Example using `curl`:

```bash
# Static file requests
curl http://localhost:8080/              # Serves public/index.html
curl http://localhost:8080/style.css     # Serves public/style.css
curl http://localhost:8080/script.js     # Serves public/script.js

# API requests
curl http://localhost:8080/api/          # Returns "Hello"
curl -X POST http://localhost:8080/api/echo -d "Hello, Server!"

# Keep-alive (multiple requests, single connection)
curl -v http://localhost:8080/ http://localhost:8080/style.css

# Request with explicit Connection: close
curl -H "Connection: close" http://localhost:8080/
```

**Apache Bench:**
```bash
# Benchmark static file serving
ab -n 10000 -c 100 http://localhost:8080/

# Benchmark API endpoint
ab -n 10000 -c 50 -p /dev/null http://localhost:8080/api/echo
```

## Limitations & Known Issues

### Current Limitations
- **Single-Threaded**: All I/O handled in one thread (CPU-bound for compute-heavy tasks)
- **Linux-Only**: Uses epoll (Linux-specific); not portable to BSD/macOS (would need kqueue)
- **No Worker Threads**: CPU-intensive operations block event loop
- **Limited API Routes**: Only 2 API endpoints (/api/ and /api/echo)
- **No HEAD Method**: HEAD requests return 405 Method Not Allowed
- **No Range Requests**: Cannot serve partial file content (no byte-range support)
- **No Caching**: No ETag or Last-Modified headers for browser caching
- **Hardcoded Port**: Always uses port 8080
- **No Logging**: Minimal debug output only
- **No Connection Limits**: Unlimited concurrent connections (depends on OS limits)

### Fixed in v0.5
- ✅ **Event-Driven Architecture**: Single-process epoll-based I/O multiplexing
- ✅ **C10K Scalability**: Handles 10,000+ concurrent connections efficiently
- ✅ **Non-Blocking I/O**: All socket operations use O_NONBLOCK
- ✅ **State Machine**: Per-connection lifecycle tracking
- ✅ **Memory Efficiency**: No process/thread overhead per connection

### Fixed in v0.4
- ✅ **Static File Serving**: Server now serves files from `public/` directory with `sendfile()`
- ✅ **Content-Type Detection**: Automatic MIME type headers based on file extensions
- ✅ **Path Traversal Protection**: URL decoding and normalization prevent directory escape
- ✅ **HTTP/1.0 Support**: Compatible with older HTTP clients and Apache Bench
- ✅ **Request Timeout**: 10-second socket timeout prevents slow client attacks

### Fixed in v0.3
- ✅ **Concurrent Connections**: Server now handles multiple clients simultaneously via `fork()`
- ✅ **Zombie Processes**: Prevented with `SIGCHLD` handler reaping children

See individual component documentation for detailed limitations and assumptions.

## Future Goals

### v0.6 — Multi-Threading Support
**Goal**: Add worker thread pool for CPU-bound operations
- Main thread handles epoll event loop
- Worker threads process requests and generate responses
- Thread-safe connection state management
- Improved performance for compute-intensive handlers

### Long-term Goals
- Configurable ports and settings
- Comprehensive logging system
- Chunked transfer encoding
- Additional HTTP methods (HEAD, PUT, DELETE, OPTIONS)
- HTTP compression support
- Performance optimizations (buffer pooling, connection pooling)

## Code Attribution

All of the C code till now is written by **Aryan Singh Thakur** only. The documentation is written with the help of AI.

Each version exists to teach one systems concept. See [docs/CHANGELOG.md](docs/CHANGELOG.md) for detailed release notes and feature history.
