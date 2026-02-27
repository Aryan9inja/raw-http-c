#ifndef CONNECTION_H
#define CONNECTION_H

#include <sys/types.h>
#include <stdint.h>
#include"httpParser.h"
typedef enum {
    READING_HEADERS,
    READING_BODY,
    PROCESSING,
    WRITING_RESPONSE,
    SENDING_FILE,
    CLOSING
}conn_state_t;

typedef struct connection {
    int fd;
    conn_state_t state;

    // persistent read buffer
    char* read_buf;
    size_t read_len;
    size_t read_cap;

    size_t parse_offset; // Indicates how much request has been parsed
    size_t header_end; // To keep header end offset
    size_t body_expected;
    size_t body_recieved;

    // persistent write buffer (lazy allocated)
    char* write_buf;
    size_t write_len;
    size_t write_sent;

    // file sending
    int file_fd;
    off_t file_offset;
    size_t file_remaining;

    httpInfo_t request;
}connection_t;

void initializeConnection(connection_t* conn, int fd);

/**
 * @returns -1 for close, EPOLLOUT for write and EPOLLIN for read.
 * It can also return bitwise values
 */
uint32_t connectionHandler(connection_t* conn, u_int32_t events);

#endif