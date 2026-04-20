/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsocket.c - Coroutine-safe blocking Socket API implementation
 *
 * KEY CONCEPT:
 *   1. All sockets set to non-blocking mode
 *   2. Read/write operations try direct execution first
 *   3. If EAGAIN/EWOULDBLOCK, register with netpoll and suspend coroutine
 *   4. On I/O ready, netpoll wakes coroutine to continue
 */

#include "xsocket.h"
#include "../base/xchecks.h"
#include "xnetpoll.h"
#include "xworker.h"
#include "../vm/xvm_internal.h"
#include "../runtime/xray_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

// ========== Utility Functions ==========

// Set socket to non-blocking mode
int xr_socket_set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}


// ========== Connection Management ==========

// Create listen socket
int xr_socket_listen(const char *host, int port, int backlog) {
    XR_DCHECK(host != NULL, "socket_listen: NULL host");
    XR_DCHECK(port > 0, "socket_listen: invalid port");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    // Set socket options
    xr_socket_set_reuseaddr(fd, true);
    xr_socket_set_nonblock(fd);

    // Bind address
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host && host[0] != '\0') {
        addr.sin_addr.s_addr = inet_addr(host);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind() failed (port %d): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    // Start listening
    if (listen(fd, backlog > 0 ? backlog : 128) < 0) {
        fprintf(stderr, "listen() failed (port %d): %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

// Blocking accept (coroutine-safe, uses yieldable protocol internally)
//
// Note: This function truly blocks until a connection arrives.
// In coroutine context, the coroutine is suspended, not the thread.
int xr_socket_accept(XrayIsolate *X, int listen_fd) {
    if (!X) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) {
        // No Runtime, use system blocking accept
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        return accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
    }

    // Get or create pollDesc
    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, listen_fd);
    if (!pd) {
        return -1;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);

        if (client_fd >= 0) {
            xr_socket_set_nonblock(client_fd);
            xr_socket_set_nodelay(client_fd, true);
            return client_fd;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Block until I/O ready (uses pthread_cond_wait, not busy-wait)
            if (xr_netpoll_block(pd, XR_POLL_READ, X)) {
                continue;
            }
            return -1;
        }

        return -1;
    }
}

// Graceful socket close
//
// Steps:
// 1. shutdown(SHUT_WR) sends FIN, notifies peer no more data
// 2. Use poll to wait for data to be sent (max 100ms)
// 3. Read remaining data peer may have sent (avoid RST)
// 4. Close socket
//
// This ensures:
// - Data fully sent to peer
// - No RST packet from close
// - curl and other clients receive response correctly
void xr_socket_close(XrayIsolate *X, int fd) {
    (void)X;
    if (fd < 0) return;

    // 1. Close write end, send FIN
    shutdown(fd, SHUT_WR);

    // 2. Use poll to wait for data send complete (max 100ms)
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, 100) > 0) {
        // 3. Read remaining data (avoid RST)
        char buf[256];
        while (recv(fd, buf, sizeof(buf), MSG_DONTWAIT) > 0) {
            // Discard data
        }
    }

    // 4. Close socket
    close(fd);
}

// ========== Data Read/Write ==========

// Blocking read (coroutine-safe, uses yieldable protocol internally)
int xr_socket_read(XrayIsolate *X, int fd, char *buf, size_t len) {
    if (!X || fd < 0 || !buf || len == 0) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) {
        // No Runtime, use system blocking read
        return (int)read(fd, buf, len);
    }

    // Get or create pollDesc
    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd) {
        return -1;
    }

    while (1) {
        ssize_t n = read(fd, buf, len);

        if (n > 0) {
            return (int)n;
        }

        if (n == 0) {
            return 0;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (xr_netpoll_block(pd, XR_POLL_READ, X)) {
                continue;
            }
            return -1;
        }

        return -1;
    }
}

// Blocking write (coroutine-safe, uses yieldable protocol internally)
int xr_socket_write(XrayIsolate *X, int fd, const char *buf, size_t len) {
    if (!X || fd < 0 || !buf || len == 0) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) {
        // No Runtime, use system blocking write
        return (int)write(fd, buf, len);
    }

    // Get or create pollDesc
    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd) {
        return -1;
    }

    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);

        if (n > 0) {
            total += n;
            if (total >= len) {
                return (int)total;
            }
            continue;
        }

        if (n == 0) {
            return total > 0 ? (int)total : -1;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (xr_netpoll_block(pd, XR_POLL_WRITE, X)) {
                continue;
            }
            return total > 0 ? (int)total : -1;
        }

        return total > 0 ? (int)total : -1;
    }

    return (int)total;
}

// Blocking readline (coroutine-safe)
int xr_socket_readline(XrayIsolate *X, int fd, char *buf, size_t maxlen) {
    if (!buf || maxlen == 0) return -1;

    size_t pos = 0;
    while (pos < maxlen - 1) {
        char c;
        int n = xr_socket_read(X, fd, &c, 1);

        if (n <= 0) {
            // EOF or error
            if (pos > 0) {
                buf[pos] = '\0';
                return (int)pos;
            }
            return n;
        }

        if (c == '\n') {
            // Line end
            buf[pos] = '\0';
            // Remove \r (if present)
            if (pos > 0 && buf[pos - 1] == '\r') {
                buf[pos - 1] = '\0';
                return (int)(pos - 1);
            }
            return (int)pos;
        }

        buf[pos++] = c;
    }

    // Line too long
    buf[pos] = '\0';
    return (int)pos;
}

// ========== Timeout Settings ==========

// Set read timeout
void xr_socket_set_read_timeout(XrayIsolate *X, int fd, int timeout_ms) {
    if (!X || fd < 0) return;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return;

    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd) return;

    int64_t deadline = 0;
    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = ts.tv_sec * 1000000000LL + ts.tv_nsec + timeout_ms * 1000000LL;
    }

    // Get current Worker's Timer Wheel
    XrTimerWheel *tw = NULL;
    XrWorker *worker = xr_current_worker();
    if (worker) {
        tw = worker->p.timer_wheel;
    }

    xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline, XR_POLL_READ, tw);
}

/*
 * Wait until fd is readable (or deadline fires) without consuming
 * bytes. Implemented as set-read-deadline + xr_netpoll_block(POLL_READ)
 * — the same primitives xr_socket_read uses, just without the final
 * read() call that would truncate a datagram on SOCK_DGRAM fds.
 *
 * The deadline is cleared on exit so callers can reuse the fd with
 * fresh timeouts. Return codes mirror the comment on the header:
 *   > 0 readable, 0 timeout, < 0 error.
 */
int xr_socket_wait_readable(XrayIsolate *X, int fd, int timeout_ms) {
    if (!X || fd < 0) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return -1;

    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd) return -1;

    // Arm read deadline. timeout_ms == 0 leaves the fd without a
    // deadline; xr_netpoll_block then sleeps until POLLIN.
    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec
                         + (int64_t)timeout_ms * 1000000LL;
        XrTimerWheel *tw = NULL;
        XrWorker *worker = xr_current_worker();
        if (worker) tw = worker->p.timer_wheel;
        xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline,
                                XR_POLL_READ, tw);
    }

    bool ready = xr_netpoll_block(pd, XR_POLL_READ, X);

    // Always clear any deadline we may have armed.
    if (timeout_ms > 0) {
        xr_netpoll_set_deadline(&runtime->netpoll, pd, 0,
                                XR_POLL_READ, NULL);
    }

    return ready ? 1 : 0;
}

/*
 * Write-side counterpart to xr_socket_wait_readable. Implementation
 * is literally identical except for XR_POLL_READ → XR_POLL_WRITE —
 * netpoll stores rseq/wseq deadlines independently, so the write
 * deadline arm/clear does not disturb any concurrent read wait on
 * the same fd.
 */
int xr_socket_wait_writable(XrayIsolate *X, int fd, int timeout_ms) {
    if (!X || fd < 0) return -1;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return -1;

    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd) return -1;

    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec
                         + (int64_t)timeout_ms * 1000000LL;
        XrTimerWheel *tw = NULL;
        XrWorker *worker = xr_current_worker();
        if (worker) tw = worker->p.timer_wheel;
        xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline,
                                XR_POLL_WRITE, tw);
    }

    bool ready = xr_netpoll_block(pd, XR_POLL_WRITE, X);

    if (timeout_ms > 0) {
        xr_netpoll_set_deadline(&runtime->netpoll, pd, 0,
                                XR_POLL_WRITE, NULL);
    }

    return ready ? 1 : 0;
}

// Set write timeout
void xr_socket_set_write_timeout(XrayIsolate *X, int fd, int timeout_ms) {
    if (!X || fd < 0) return;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) return;

    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd) return;

    int64_t deadline = 0;
    if (timeout_ms > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = ts.tv_sec * 1000000000LL + ts.tv_nsec + timeout_ms * 1000000LL;
    }

    // Get current Worker's Timer Wheel
    XrTimerWheel *tw = NULL;
    XrWorker *worker = xr_current_worker();
    if (worker) {
        tw = worker->p.timer_wheel;
    }

    xr_netpoll_set_deadline(&runtime->netpoll, pd, deadline, XR_POLL_WRITE, tw);
}

// ========== Yieldable API Implementation ==========

// Forward declaration (already declared in header)

// Yieldable accept (supports coroutine yield)
XrCFuncResult xr_socket_accept_yieldable(XrayIsolate *X, XrAcceptState *state) {
    if (!X || !state) return XR_CFUNC_ERROR;

    XrRuntime *runtime = (XrRuntime *)X->vm.runtime;
    if (!runtime) {
        // No Runtime, use blocking accept
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        state->result_fd = accept(state->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        return (state->result_fd >= 0) ? XR_CFUNC_DONE : XR_CFUNC_ERROR;
    }

    // Get or create pollDesc
    if (!state->pd) {
        state->pd = xr_netpoll_open(&runtime->netpoll, state->listen_fd);
        if (!state->pd) {
            return XR_CFUNC_ERROR;
        }
    }

    // Try non-blocking accept
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(state->listen_fd, (struct sockaddr *)&client_addr, &addr_len);

    if (client_fd >= 0) {
        // Success
        xr_socket_set_nonblock(client_fd);
        xr_socket_set_nodelay(client_fd, true);
        state->result_fd = client_fd;
        return XR_CFUNC_DONE;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Yield directly (no block_yieldable double-call)
        return xr_yield_for_io(X, state->listen_fd, XR_WAIT_READ, -1,
                               xr_socket_accept_continue, state, NULL);
    }

    // Other errors
    state->result_fd = -1;
    return XR_CFUNC_ERROR;
}

// Accept continuation (new signature: added result param)
XrCFuncResult xr_socket_accept_continue(XrayIsolate *X, int status, void *ctx, XrValue *result) {
    (void)result;  // accept returns result via state->result_fd
    XrAcceptState *state = (XrAcceptState *)ctx;
    if (!state) return XR_CFUNC_ERROR;

    // Check resume status
    if (status == XR_RESUME_TIMEOUT || status == XR_RESUME_CANCELLED) {
        state->result_fd = -1;
        return XR_CFUNC_ERROR;
    }

    // Continue trying accept
    return xr_socket_accept_yieldable(X, state);
}

// ========== Non-blocking Try API Implementation ==========

// Non-blocking accept try
XrIOTryResult xr_socket_accept_try(XrayIsolate *X, int listen_fd) {
    (void)X;
    XrIOTryResult result = {false, -1, 0};

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd >= 0) {
        // Success: set new connection to non-blocking
        xr_socket_set_nonblock(client_fd);
        xr_socket_set_nodelay(client_fd, true);
        result.ready = true;
        result.value = client_fd;
        return result;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No connection, need to wait
        result.ready = false;
        return result;
    }

    // Other errors
    result.ready = true;
    result.value = -1;
    result.error = errno;
    return result;
}

// Non-blocking read try
XrIOTryResult xr_socket_read_try(XrayIsolate *X, int fd, char *buf, size_t len) {
    (void)X;
    XrIOTryResult result = {false, 0, 0};

    ssize_t n = read(fd, buf, len);

    if (n > 0) {
        result.ready = true;
        result.value = (int)n;
        return result;
    }

    if (n == 0) {
        // EOF
        result.ready = true;
        result.value = 0;
        return result;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No data, need to wait
        result.ready = false;
        return result;
    }

    // Other errors
    result.ready = true;
    result.value = -1;
    result.error = errno;
    return result;
}

// Non-blocking write try
XrIOTryResult xr_socket_write_try(XrayIsolate *X, int fd, const char *buf, size_t len) {
    (void)X;
    XrIOTryResult result = {false, 0, 0};

    ssize_t n = write(fd, buf, len);

    if (n >= 0) {
        result.ready = true;
        result.value = (int)n;
        return result;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Buffer full, need to wait
        result.ready = false;
        return result;
    }

    // Other errors
    result.ready = true;
    result.value = -1;
    result.error = errno;
    return result;
}

