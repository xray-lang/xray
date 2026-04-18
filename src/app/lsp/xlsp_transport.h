/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_transport.h - LSP transport layer (stdio, non-blocking)
 *
 * KEY CONCEPT:
 *   Base-protocol framing over stdio with an incremental, non-blocking
 *   frame parser. The server main loop registers read_fd with xr_poll
 *   and reacts to readable-stdin events by calling
 *   xlsp_transport_try_read() until it reports would_block; each
 *   complete LSP message is returned as a heap string that the caller
 *   must free.
 *
 *   There is intentionally NO blocking read API — the server is
 *   single main-thread / many-workers, and blocking on stdin would
 *   starve completed background tasks and diagnostic debounce timers.
 */

#ifndef XLSP_TRANSPORT_H
#define XLSP_TRANSPORT_H

#include <stddef.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

// Transport context.
//
// read_buf is a single sliding buffer that accumulates whatever data
// arrived from stdin so far. As soon as the full header block
// "Content-Length: N\r\n\r\n" followed by N body bytes is present,
// try_read() hands back a freshly allocated copy of the body and
// memmove()s the rest of the buffer down so the next frame can start
// parsing without reallocation.
typedef struct XrLspTransport {
    int    read_fd;                 // Input fd (non-blocking)
    int    write_fd;                // Output fd (retries on EAGAIN)

    char  *read_buf;                // Incremental read accumulator
    size_t read_cap;                // read_buf capacity
    size_t read_len;                // Valid bytes in read_buf

    // Parser state between try_read() calls. Cleared each time a
    // complete frame is delivered.
    int    pending_content_length;  // -1 until Content-Length header parsed
    size_t header_end;              // Offset just past "\r\n\r\n"

    bool   connected;               // False after EOF / fatal read error
} XrLspTransport;

// Create an stdio transport (stdin -> read_fd, stdout -> write_fd).
// Sets stdin to non-blocking and disables stdout buffering.
XR_FUNC XrLspTransport *xlsp_transport_stdio(void);

// Destroy the transport. Does NOT close stdin/stdout — they outlive us.
XR_FUNC void xlsp_transport_free(XrLspTransport *t);

// Try to read one complete LSP message without blocking.
//   * On success: returns malloc'd JSON body (null-terminated), and
//     sets *out_len / *would_block=false.
//   * No complete frame yet: returns NULL, *would_block=true.
//   * EOF / fatal error: returns NULL, *would_block=false, and
//     `connected` becomes false.
XR_FUNC char *xlsp_transport_try_read(XrLspTransport *t, size_t *out_len, bool *would_block);

// Write one LSP message with the "Content-Length: N\r\n\r\n" header.
// Uses a retry loop for EAGAIN/EINTR so it is logically blocking on
// back-pressure but never spins on a broken pipe (silently returns;
// the next try_read() will surface EOF to the caller).
XR_FUNC void xlsp_transport_write(XrLspTransport *t, const char *json, size_t len);

// Input fd for poll registration.
XR_FUNC int xlsp_transport_get_fd(XrLspTransport *t);

// False once EOF / fatal read error has been observed.
XR_FUNC bool xlsp_transport_is_connected(XrLspTransport *t);

#endif // XLSP_TRANSPORT_H
