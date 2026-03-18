# server.c Documentation

## Overview

`server.c` is the main entry point and orchestrator of the HTTP/1.1 server. In v0.5, it implements an event-driven architecture using Linux epoll for I/O multiplexing, managing multiple concurrent connections in a single process with non-blocking sockets.

### v0.5 Status
Complete rewrite for **event-driven architecture**:
- **Event Loop**: Single-process epoll-based I/O multiplexing
- **Non-Blocking Sockets**: Server and client sockets configured with O_NONBLOCK
- **State Machine**: Per-connection state tracking via `connection_t` structures
- **Scalable**: Handles 10,000+ concurrent connections (C10K capable)
- **Memory Efficient**: No process/thread creation overhead per connection
- **Document Root FD**: Passed to all connections for secure file access

## Key Responsibilities

1. **TCP Socket Setup**: Creates, binds, and listens on port 8080
2. **Document Root Setup**: Opens `public/` directory descriptor for secure file access
3. **Epoll Configuration**: Creates epoll instance and registers server socket
4. **Event Loop**: Infinite loop waiting for socket events via epoll_wait()
5. **Connection Acceptance**: Non-blocking accept() loop for new connections
6. **Connection Management**: Allocates and initializes `connection_t` structures
7. **Event Dispatching**: Routes events to connectionHandler() for processing
8. **Interest Updates**: Modifies epoll interest based on handler return values
9. **Resource Cleanup**: Closes connections and frees memory when state is CLOSING

## Core Architecture

### Event Loop Flow

```
┌─────────────────────────────────────────────┐
│           epoll_wait()                      │
│     (blocks until events ready)             │
└─────────────┬───────────────────────────────┘
              │
              ▼
    ┌─────────────────────┐
    │  Events Available?  │
    └─────────┬───────────┘
              │
        ┌─────┴─────┐
        │           │
        ▼           ▼
 [Server Socket]  [Client Socket(s)]
        │              │
        │              ▼
        │    ┌──────────────────────┐
        │    │ connectionHandler()  │
        │    │  (state machine)     │
        │    └──────────┬───────────┘
        │               │
        │         Returns event mask
        │               │
        ▼               ▼
 accept() loop   ┌─────────────────┐
   (EAGAIN)      │ EPOLL_CTL_MOD   │
        │        │  or             │
        │        │ EPOLL_CTL_DEL   │
        │        │  (close)        │
        │        └─────────────────┘
        │
        ▼
 Create connection_t
 Set non-blocking
 initializeConnection()
 EPOLL_CTL_ADD
        │
        └─────────────┐
                      │
                      ▼
              Back to epoll_wait()
```

### Main Function Structure

```c
int main() {
    // 1. Setup
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(server_fd, F_SETFL, O_NONBLOCK);  // Non-blocking
    
    int docroot_fd = open("public", O_RDONLY | O_DIRECTORY);
    
    // 2. Bind and listen
    bind(server_fd, ...);
    listen(server_fd, 50);  // Backlog of 50
    
    // 3. Create epoll instance
    int epoll_fd = epoll_create(1);
    
    // 4. Register server socket for EPOLLIN
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);
    
    // 5. Event loop
    struct epoll_event events[100];
    while (1) {
        int n = epoll_wait(epoll_fd, events, 100, -1);
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server_fd) {
                // Accept new connections
                handle_accepts(server_fd, epoll_fd, docroot_fd);
            } else {
                // Handle client I/O
                handle_client_event(events[i], epoll_fd);
            }
        }
    }
}
```

## Key Components

### 1. Socket Initialization

**Non-Blocking Server Socket:**
```c
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
int flags = fcntl(server_fd, F_GETFL, 0);
fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
```

- Server socket set to non-blocking mode
- accept() returns EAGAIN when no connections pending
- Allows draining accept queue in a loop

**Socket Options:**
```c
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

- SO_REUSEADDR: Allow immediate port reuse after restart
- Prevents "Address already in use" errors

### 2. Epoll Setup

**Creating Epoll Instance:**
```c
int epoll_fd = epoll_create(1);  // Size hint (ignored in modern kernels)
```

**Registering Server Socket:**
```c
struct epoll_event ev;
ev.events = EPOLLIN;           // Monitor for incoming connections
ev.data.fd = server_fd;        // Store server socket fd
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);
```

- EPOLLIN: Ready for reading (new connections available)
- data.fd: Used to identify server socket in event loop

### 3. Accept Loop

**Non-Blocking Accept:**
```c
while ((new_socket = accept(server_fd, &address, &addrlen)) != -1) {
    // Set client socket non-blocking
    int flags = fcntl(new_socket, F_GETFL, 0);
    fcntl(new_socket, F_SETFL, flags | O_NONBLOCK);
    
    // Allocate connection structure
    connection_t* conn = malloc(sizeof(connection_t));
    initializeConnection(conn, new_socket, docroot_fd);
    
    // Register with epoll
    struct epoll_event conn_ev;
    conn_ev.events = EPOLLIN;
    conn_ev.data.ptr = conn;  // Store connection pointer
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_socket, &conn_ev);
}

// Check for EAGAIN (no more connections)
if (errno != EAGAIN && errno != EWOULDBLOCK) {
    perror("accept");
}
```

**Key Points:**
- Loop drains all pending connections from accept queue
- Each client socket configured as non-blocking
- Connection structure allocated and initialized
- Connection pointer stored in epoll_event.data.ptr
- Initial interest is EPOLLIN (ready to read request)

### 4. Event Dispatching

**Processing Client Events:**
```c
connection_t* conn = events[i].data.ptr;
uint32_t epoll_events = events[i].events;

uint32_t mask = connectionHandler(conn, epoll_events);

if (mask == UINT32_MAX) {
    // Connection closed
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    closeConnection(conn);
} else {
    // Update epoll interest
    struct epoll_event temp_ev;
    temp_ev.data.ptr = conn;
    temp_ev.events = mask;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &temp_ev);
}
```

**Event Mask Values:**
- `EPOLLIN`: Connection ready for reading
- `EPOLLOUT`: Connection ready for writing
- `EPOLLIN | EPOLLOUT`: Ready for both (rare)
- `UINT32_MAX`: Close connection (special value)

**Handler Return Values:**
The connectionHandler() returns event mask for next interest:
- Return EPOLLIN → Wait for more data to read
- Return EPOLLOUT → Wait for socket writable
- Return UINT32_MAX → Close connection

### 5. Connection Lifecycle

**Initialization:**
```c
void initializeConnection(connection_t* conn, int fd, int fileRootFd);
```

- Allocates read buffer (4KB initial)
- Allocates write buffer (64KB)
- Sets initial state to READING_HEADERS
- Stores root_fd for file access

**State Transitions:**
1. **READING_HEADERS** → Read until "\r\n\r\n" found
2. **READING_BODY** → Read Content-Length bytes (if needed)
3. **PROCESSING** → Parse URL, route request, generate response
4. **WRITING_RESPONSE** → Write HTTP headers and body
5. **SENDING_FILE** → Use sendfile() for static files
6. **CLOSING** → Clean up and free resources

**Cleanup:**
```c
void closeConnection(connection_t* conn);
```

- Closes client socket fd
- Closes file_fd if opened
- Frees read_buf and write_buf
- Frees connection_t structure

## Non-Blocking I/O Patterns

### Partial Reads

```c
ssize_t valread = read(fd, buffer + offset, remaining);

if (valread > 0) {
    offset += valread;  // Made progress
    // Continue reading if needed
} else if (valread == 0) {
    // Connection closed by client
    return CLOSE_CONNECTION;
} else {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No data available, wait for next EPOLLIN
        return EPOLLIN;
    }
    // Real error
    return CLOSE_CONNECTION;
}
```

### Partial Writes

```c
ssize_t sent = write(fd, buffer + offset, remaining);

if (sent > 0) {
    offset += sent;
    if (offset < total) {
        // More to write, wait for EPOLLOUT
        return EPOLLOUT;
    }
    // Write complete
    return next_state();
} else {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Socket buffer full, wait for EPOLLOUT
        return EPOLLOUT;
    }
    // Error
    return CLOSE_CONNECTION;
}
```

## Memory Management

### Per-Connection Buffers

**Read Buffer:**
- Initial: 4KB (READ_BUFFER_SIZE)
- Grows dynamically as needed
- Maximum: 8KB for headers (MAX_HEADER_SIZE)

**Write Buffer:**
- Initial: 64KB (MIN_RESPONSE_BUFFER)
- Holds complete HTTP response headers/body
- For static files, only headers (body via sendfile)

**Lifecycle:**
- Allocated in initializeConnection()
- Reused across multiple requests (keep-alive)
- Freed in closeConnection()

### File Descriptors

**Root Directory FD:**
```c
int docroot_fd = open("public", O_RDONLY | O_DIRECTORY);
```

- Opened once at startup
- Shared across all connections
- Never closed (live for server lifetime)

**Per-Request File FD:**
```c
conn->file_fd = openat(conn->root_fd, path, O_RDONLY);
```

- Opened in handler when serving static file
- Closed after sendfile() complete
- Prevents directory traversal via openat()

## Error Handling

### Accept Errors

```c
if (errno != EAGAIN && errno != EWOULDBLOCK) {
    perror("accept");
    // Continue event loop (don't exit)
}
```

- EAGAIN/EWOULDBLOCK are normal (no more connections)
- Other errors logged but server continues

### Epoll Errors

```c
if (epoll_wait(...) == -1) {
    perror("epoll_wait");
    break;  // Exit event loop
}
```

- epoll_wait() failure is fatal
- Server exits on epoll errors

### Connection Errors

Handled inside connectionHandler():
- Read errors → close connection
- Write errors → close connection
- Parse errors → send error response, close
- Invalid state → close connection

## Performance Characteristics

### Scalability

- **C10K Capable**: Handles 10,000+ concurrent connections
- **Single Process**: No fork/thread overhead
- **Minimal Context Switches**: Only on I/O events
- **Memory Efficient**: ~70KB per idle connection

### Benchmarks

**Test Environment:**
- AMD Ryzen 5 5600H (12 cores)
- Linux with epoll
- Apache Bench (ab)

**Results:**
- Static file serving: ~14,300 req/sec (c=10)
- API endpoints: ~14,300 req/sec (c=10)
- Mean latency: 0.7ms
- Zero failed requests

**Comparison to v0.4 (fork-based):**
- 38% higher throughput
- 30% lower latency
- 90% less memory per connection

## Limitations

### Current Limitations

1. **Single-Threaded**: CPU-bound operations block event loop
2. **Linux-Only**: Uses epoll (not portable to BSD/macOS)
3. **No Timeout Handling**: Idle connections persist indefinitely
4. **No Connection Limits**: Can exhaust file descriptors
5. **No Priority**: All connections treated equally

### Future Improvements

1. **Worker Threads**: Offload CPU-intensive tasks to thread pool
2. **Connection Timeouts**: Close idle connections after timeout
3. **Max Connections**: Limit concurrent connections
4. **Load Balancing**: Distribute connections across worker threads
5. **Portable I/O**: Abstract epoll/kqueue/io_uring

## Code Example

### Minimal Event Loop

```c
while (1) {
    int n = epoll_wait(epoll_fd, events, 100, -1);
    
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == server_fd) {
            // New connections
            while ((fd = accept(server_fd, ...)) != -1) {
                set_nonblocking(fd);
                connection_t* conn = new_connection(fd);
                epoll_add(epoll_fd, fd, EPOLLIN, conn);
            }
        } else {
            // Client I/O
            connection_t* conn = events[i].data.ptr;
            uint32_t mask = handle_connection(conn, events[i].events);
            
            if (mask == CLOSE) {
                epoll_del(epoll_fd, conn->fd);
                free_connection(conn);
            } else {
                epoll_mod(epoll_fd, conn->fd, mask, conn);
            }
        }
    }
}
```

## Debugging Tips

### Check Connection Count

```bash
# Count active connections
lsof -p $(pgrep server) | grep TCP | wc -l
```

### Monitor epoll Events

Add debug prints in event loop:
```c
printf("epoll_wait returned %d events\n", n);
for (int i = 0; i < n; i++) {
    printf("Event: fd=%d, events=0x%x\n", fd, events[i].events);
}
```

### Track State Machine

Add logging in connectionHandler():
```c
printf("Connection %d: %s -> %s\n", 
       conn->fd, state_name(old_state), state_name(conn->state));
```

## Related Documentation

- [connection.md](connection.md) - Connection state machine details
- [httpParser.md](httpParser.md) - HTTP parsing implementation
- [handlers.md](handlers.md) - Request routing and response generation
- [BENCHMARKS.md](BENCHMARKS.md) - Performance measurements

## Version History

- **v0.5**: Event-driven architecture with epoll
- **v0.4**: Static file serving with sendfile()
- **v0.3**: Process-based concurrency with fork()
- **v0.2**: Keep-alive connection support
- **v0.1**: Basic HTTP/1.1 parsing and response
