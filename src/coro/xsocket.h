/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsocket.h - Coroutine-safe blocking Socket API
 *
 * KEY CONCEPT:
 *   1. Provides POSIX-like blocking API
 *   2. When coroutine blocks, netpoll suspends it (doesn't block thread)
 *   3. Integrated with GMP scheduler
 *
 * USAGE:
 *   int client_fd = xr_socket_accept(X, listen_fd);
 *   int n = xr_socket_read(X, fd, buf, len);
 *   int n = xr_socket_write(X, fd, buf, len);
 */

#ifndef XSOCKET_H
#define XSOCKET_H

#include <stddef.h>
#include "../base/xdefs.h"
#include "../os/os_net.h"

struct XrayIsolate;

// ========== Connection Management ==========

// Create listen socket
// Returns listen_fd, -1 on failure
XR_FUNC int xr_socket_listen(const char *host, int port, int backlog);

// Blocking accept (coroutine-safe)
// Coroutine suspends until new connection arrives
// Returns client_fd, -1 on failure
XR_FUNC int xr_socket_accept(struct XrayIsolate *X, int listen_fd);

// Close socket
XR_FUNC void xr_socket_close(struct XrayIsolate *X, int fd);

// ========== Data Read/Write ==========

// Blocking read (coroutine-safe)
// Coroutine suspends until data readable
// Returns bytes read, 0 = EOF, -1 = error
XR_FUNC int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);

// Blocking write (coroutine-safe)
// Coroutine suspends until data writable
// Returns bytes written, -1 = error
XR_FUNC int xr_socket_write(struct XrayIsolate *X, int fd, const char *buf, size_t len);

// Blocking readline (coroutine-safe)
// Reads one line (ending with \n or \r\n)
// Returns bytes read (excluding newline), -1 = error
XR_FUNC int xr_socket_readline(struct XrayIsolate *X, int fd, char *buf, size_t maxlen);

// ========== Timeout Settings ==========

// Set read timeout (milliseconds, 0 = no timeout)
XR_FUNC void xr_socket_set_read_timeout(struct XrayIsolate *X, int fd, int timeout_ms);

// Set write timeout (milliseconds, 0 = no timeout)
XR_FUNC void xr_socket_set_write_timeout(struct XrayIsolate *X, int fd, int timeout_ms);

/*
 * Wait until fd is readable or the deadline fires, without consuming
 * any bytes. Intended for datagram sockets (UDP, raw) where an
 * xr_socket_read into a small buffer would truncate the datagram per
 * POSIX recv semantics. Also useful when a caller wants to peek at
 * fd state before deciding which read buffer size to allocate.
 *
 * timeout_ms > 0 arms a deadline via the timer wheel. timeout_ms == 0
 * waits indefinitely until readable (not recommended).
 *
 * Returns:
 *   > 0 — fd is readable (caller should recvfrom / read as needed)
 *     0 — deadline fired without data
 *   < 0 — error (e.g. fd closed during wait, netpoll registration failure)
 */
XR_FUNC int xr_socket_wait_readable(struct XrayIsolate *X, int fd, int timeout_ms);

/*
 * Wait until fd is writable or the deadline fires, without writing
 * any bytes. Symmetric counterpart to xr_socket_wait_readable — uses
 * the same netpoll + timer-wheel primitives with XR_POLL_WRITE.
 *
 * Primary use case: coroutine-friendly non-blocking TCP connect.
 * After an EINPROGRESS connect(), a writable event on the socket
 * signals the connection has either succeeded (check SO_ERROR == 0)
 * or failed with a specific errno. Same pattern is used inside
 * stdlib/net/io.c's xr_io_connect for the cluster path.
 *
 * Return codes mirror the reader variant:
 *   > 0 — fd is writable (caller should check SO_ERROR / proceed)
 *     0 — deadline fired
 *   < 0 — error (fd closed during wait, netpoll registration failed)
 */
XR_FUNC int xr_socket_wait_writable(struct XrayIsolate *X, int fd, int timeout_ms);

// ========== Utility Functions ==========

// Set socket to non-blocking mode
XR_FUNC int xr_socket_set_nonblock(int fd);

// Set TCP_NODELAY (wrapper for compatibility)
static inline int xr_socket_set_nodelay_simple(int fd) {
    int flag = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

// Set SO_REUSEADDR (wrapper for compatibility)
static inline int xr_socket_set_reuseaddr_simple(int fd) {
    int flag = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
}

// ========== Yieldable API (supports coroutine yield) ==========

#include "xnetpoll.h"
#include "xyieldable.h"

// Accept state
typedef struct XrAcceptState {
    int listen_fd;   // Listen fd
    XrPollDesc *pd;  // Poll descriptor
    int result_fd;   // Result fd
} XrAcceptState;

// Yieldable accept (supports coroutine yield)
//
// Returns:
//   XR_CFUNC_DONE: success, result in state->result_fd
//   XR_CFUNC_BLOCKED: need to wait, coroutine should yield
//   XR_CFUNC_ERROR: error
XR_FUNC XrCFuncResult xr_socket_accept_yieldable(struct XrayIsolate *X, XrAcceptState *state);

// Accept continuation (new signature: added result param)
XR_FUNC XrCFuncResult xr_socket_accept_continue(struct XrayIsolate *X, int status, void *ctx,
                                                XrValue *result);

// ========== Non-blocking Try API (hybrid: C-level try + xray yield) ==========

// I/O try result
// ready=true means operation succeeded, ready=false means need to wait
typedef struct XrIOTryResult {
    bool ready;  // Whether ready
    int value;   // Result value (fd/bytes)
    int error;   // Error code (0 = no error)
} XrIOTryResult;

// Non-blocking accept try
// Returns immediately, no blocking. If no connection, returns ready=false
XR_FUNC XrIOTryResult xr_socket_accept_try(struct XrayIsolate *X, int listen_fd);

// Non-blocking read try
// Returns immediately, no blocking. If no data, returns ready=false
XR_FUNC XrIOTryResult xr_socket_read_try(struct XrayIsolate *X, int fd, char *buf, size_t len);

// Non-blocking write try
// Returns immediately, no blocking. If buffer full, returns ready=false
XR_FUNC XrIOTryResult xr_socket_write_try(struct XrayIsolate *X, int fd, const char *buf,
                                          size_t len);

// Register I/O wait (called before xray layer yield)
// Register fd with netpoll, coroutine can retry after resume
XR_FUNC void xr_socket_register_wait(struct XrayIsolate *X, int fd, int mode);

#endif  // XSOCKET_H
