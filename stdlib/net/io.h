/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * io.h - Coroutine-friendly I/O layer
 *
 * KEY CONCEPT:
 *   Unified I/O interface built on netpoll. All I/O operations
 *   automatically yield when blocked and resume when ready.
 */

#ifndef XR_STDLIB_NET_IO_H
#define XR_STDLIB_NET_IO_H

#include "../../src/base/xdefs.h"
#include "tls.h"
#include "xneterror.h"
#include "../../src/coro/xnetpoll.h"
#include <stddef.h>
#include <stdbool.h>
#ifdef XR_OS_WINDOWS
// os_net.h provides a struct iovec compatibility shim alongside
// the WSASend-backed writev emulation used by callers of
// xr_io_writev on Windows.
#include "../../src/os/os_net.h"
#else
#include <sys/uio.h>
#endif

/* ========== I/O Connection Context ========== */

typedef struct XrIOConn {
    int fd;                 // Socket file descriptor
    XrPollDesc *pd;         // Netpoll descriptor
    XrTlsConn *tls;         // TLS connection (optional)
    bool is_tls;            // Whether TLS enabled
    int timeout_ms;         // Default timeout (milliseconds)
    XrNetError last_error;  // Last error
} XrIOConn;

/* ========== Global Initialization ========== */

// Initialize I/O layer
// Must be called before using other APIs
XR_FUNC void xr_io_init(void);

/* Bind I/O layer to an external netpoll (e.g. runtime's netpoll).
 * Avoids creating a duplicate kqueue/epoll instance.
 * Must be called before any I/O operations. */
XR_FUNC void xr_io_init_with_netpoll(XrNetpoll *np);

// Shutdown I/O layer
XR_FUNC void xr_io_shutdown(void);

// Get active netpoll instance (owned or external)
XR_FUNC XrNetpoll *xr_io_get_netpoll(void);

/* ========== Connection API ========== */

/*
 * Create TCP connection (coroutine-friendly)
 *
 * host: Hostname or IP
 * port: Port number
 * timeout_ms: Connection timeout (milliseconds)
 *
 * Returns: Connection context, NULL on failure
 *
 * Note: This function auto-yields until connection completes or times out
 */
XR_FUNC XrIOConn *xr_io_connect(const char *host, int port, int timeout_ms);

/*
 * Create TLS connection (coroutine-friendly)
 *
 * host: Hostname (used for SNI)
 * port: Port number
 * timeout_ms: Connection timeout
 *
 * Returns: Connection context, NULL on failure
 */
XR_FUNC XrIOConn *xr_io_connect_tls(const char *host, int port, int timeout_ms);

/*
 * Create TLS connection using a caller-supplied TLS context.
 *
 * Unlike xr_io_connect_tls which binds the connection to the global
 * g_io.tls_ctx (system-default trust store), this variant lets modules
 * pin a per-service context — e.g. a cluster node with a private CA or
 * mTLS client certificate configured on `ctx`.
 *
 * Ownership of `ctx` stays with the caller. The context must outlive
 * every returned XrIOConn; freeing it earlier is use-after-free.
 *
 * Returns: coroutine-ready XrIOConn on success, NULL on connect /
 * handshake / verify failure.
 */
XR_FUNC XrIOConn *xr_io_connect_tls_with_ctx(XrTlsContext *ctx, const char *host, int port,
                                             int timeout_ms);

// Close connection
XR_FUNC void xr_io_close(XrIOConn *conn);

/* ========== Read/Write API ========== */

/*
 * Read data (coroutine-friendly)
 *
 * conn: Connection context
 * buf: Buffer
 * len: Expected read length
 *
 * Returns: Actual bytes read, 0=closed, -1=error
 *
 * Note: This function auto-yields until data available or timeout
 */
XR_FUNC int xr_io_read(XrIOConn *conn, void *buf, size_t len);

/*
 * Read exact length of data (coroutine-friendly)
 * Blocks until buffer filled or error
 */
XR_FUNC int xr_io_read_full(XrIOConn *conn, void *buf, size_t len);

/*
 * Write data (coroutine-friendly)
 *
 * conn: Connection context
 * buf: Data
 * len: Length
 *
 * Returns: Actual bytes written, -1=error
 */
XR_FUNC int xr_io_write(XrIOConn *conn, const void *buf, size_t len);

/*
 * Write all data (coroutine-friendly)
 * Blocks until all written or error
 */
XR_FUNC int xr_io_write_all(XrIOConn *conn, const void *buf, size_t len);

/*
 * Scatter-gather write (coroutine-friendly, plain TCP only)
 * Writes multiple buffers in one syscall, reducing copies.
 *
 * iov: Array of iovec structs {base, len}
 * iovcnt: Number of iovec entries
 *
 * Returns: Total bytes written, -1 on error
 */
XR_FUNC int xr_io_writev(XrIOConn *conn, const struct iovec *iov, int iovcnt);

/* ========== Server API ========== */

/*
 * Create listening socket
 *
 * addr: Bind address (NULL or "0.0.0.0" for all interfaces)
 * port: Port number
 * backlog: Connection queue length
 *
 * Returns: Listen fd, -1 on failure
 */
XR_FUNC int xr_io_listen(const char *addr, int port, int backlog);

/*
 * Accept connection (coroutine-friendly)
 *
 * listen_fd: Listening socket
 *
 * Returns: New connection context, NULL on failure
 *
 * Note: This function auto-yields until new connection arrives
 */
XR_FUNC XrIOConn *xr_io_accept(int listen_fd);

/*
 * Accept + server-side TLS handshake (coroutine-friendly).
 *
 * listen_fd: Listening socket.
 * ctx:       Server TLS context built with xr_tls_context_new_server
 *            (plus optional CA bundle for mTLS). Must outlive every
 *            returned XrIOConn; freeing it earlier is use-after-free.
 *
 * Performs the plain xr_io_accept() first, then wraps the accepted fd
 * with `ctx` and drives SSL_accept to completion. On any failure the
 * fd is closed and NULL is returned — the caller never sees a
 * half-wrapped XrIOConn.
 *
 * Returns: coroutine-ready XrIOConn on success, NULL otherwise.
 */
XR_FUNC XrIOConn *xr_io_accept_tls_with_ctx(int listen_fd, XrTlsContext *ctx);

/*
 * Wrap an existing file descriptor into an XrIOConn.
 * Caller is responsible for fd lifecycle before this call.
 * Sets non-blocking mode and TCP_NODELAY automatically.
 *
 * Returns: Connection context, NULL on failure
 */
XR_FUNC XrIOConn *xr_io_conn_from_fd(int fd, int timeout_ms);

/* ========== Utility Functions ========== */

// Set connection timeout
XR_FUNC void xr_io_set_timeout(XrIOConn *conn, int timeout_ms);

// Get last error code
XR_FUNC XrNetError xr_io_get_error(XrIOConn *conn);

// Set socket to non-blocking mode
XR_FUNC int xr_io_set_nonblocking(int fd);

/* ========== Coroutine Integration API ========== */

// Set current thread's VM instance (for coroutine scheduling)
// Call before I/O operations in coroutine mode
XR_FUNC void xr_io_set_isolate(struct XrayIsolate *X);

// Get current thread's VM instance
XR_FUNC struct XrayIsolate *xr_io_get_isolate(void);

#endif  // XR_STDLIB_NET_IO_H
