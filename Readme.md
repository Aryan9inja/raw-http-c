# RAW HTTP SERVER IN C
This is a doc for my future self.
What I want to do is to create a HTTP server in C.

## What I have done so far
- Created a basic TCP server that listens on a port and accepts connections.
- Currently the connection is closed immediately after being accepted.

# Goal and Scope [Current Version]
## Create parsers for HTTP requests
- Works on HTTP/1.x only for now.
- No chuncked transfer encoding yet.
- One connection, multiple requests (keep-alive).
- Should be binary safe (able to handle any byte sequence).
- Should be able to handle large payloads incrementally (future: streaming / partial body handling).
- Should be able to handle pipelined requests.

## Core assumptions
- TCP stream is reliable and in order.
- Requests may arive fragmented or combined.
- Parser does not own the memory buffer, it just reads from it.
- Parser data is invalid after the buffer is freed or modified.

## Buffer Model
- One dynamic buffer per connection.
- Buffer can contain partial or multiple requests.
- Bytes are consumed from the buffer as request completes.
- By bytes consumed, I mean the parser will indicate how many bytes were used for the completed request.
- Pointers like read-offset and parse-offset will be used to track progress.
- After a request is fully parsed, the buffer is advanced by the number of bytes consumed.
- Remaining bytes are preserved for next request parsing.

## Request Boundary Detection
- Detect end of headers by looking for double CRLF (`\r\n\r\n`).
- Determine body length using `Content-Length` header.
- For requests without body (e.g. GET), consider headers end as request end.
- For requests with body, wait until full body is received based on `Content-Length`.
- Handle pipelined requests by continuing to parse remaining buffer after completing a request.

## Parsing Responsibilities
### Header Parser
- Extract method, path, version from request line.
- Parse headers into key-value pairs.
- Will return method, path, version as strings.
- Will return headers as a list of key-value pairs.
- Will also return content-length and content-type if present.

### Body Parser
- Called only if `Content-Length` is present and greater than 0.
- Use `Content-Type` header to determine how to parse body (e.g. JSON, form data). Currently just plain text.
- Binary safe: should handle any byte sequence.
- Body data is treated as raw bytes and is not null-terminated.

## Memory Ownership Contract
### Who owns the buffer?
- The buffer is owned by the connection handler.
- The parser functions will not allocate or free memory for the buffer.
- The parser functions will return pointers into the existing buffer.

### Who owns the parsed data?
- Parsed fields are views into the buffer
- No individual field should ever be freed
- Only the buffer is freed (by connection handler)

### When does memory become invalid?
- The parsed data becomes invalid when the buffer is modified (e.g. bytes are consumed).
- The parsed data also becomes invalid when the connection is closed and buffer is freed.

### What must the caller not do?
- The caller must not free or modify the buffer while parsed data is still in use.
- The caller must not assume parsed data remains valid after modifying the buffer.
- The caller must not use parsed data after closing the connection and freeing the buffer.
- The caller must not modify the buffer in a way that invalidates parsed data (e.g. reallocating or freeing).
- The caller must not pass a NULL or invalid buffer pointer to the parser functions.
- The caller must not assume the parser functions will handle memory allocation for parsed data.
- The caller must not use parsed data after the next parse call, as it may be overwritten.

## Request Lifecycle
1. Connection accepted, buffer initialized.
2. Data received, appended to buffer.
3. Detect Header Boundary.
4. Parse Headers.
5. If body present, wait for full body.
6. Parse Body.
7. Process Request.
8. Advance buffer by consumed bytes.
9. Repeat from step 2 for next request.

## Non-Goals/Future Work
- Chunked transfer encoding.
- Multipart Parsing.
- Keep-Alive optimizations.
- Concurrency Model.

## Failure Modes/Expected Errors
- Incomplete headers.
- Invalid content-length.
- Body shorter than content-length.
- Oversized headers/body (exceeding buffer limits).