/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * io.h - Coroutine-friendly I/O layer (script-binding side)
 *
 * KEY CONCEPT:
 *   This layer wraps platform sockets + netpoll into a single XrIOConn
 *   handle that read/write/close paths can drive without needing the
 *   caller to thread isolate, runtime, and netpoll references through
 *   every call site. The handle is captured at create time so that
 *   read/write are pure conn-only operations.
 *
 *   IO state used to live in process-global g_io and a thread-local
 *   tls_isolate; both have been removed in favour of resolving the
 *   active runtime / netpoll / DNS cache from the captured isolate.
 *   That makes "run two isolates side-by-side" sound by construction
 *   and removes the thread-local that hid lifetime bugs.
 */

#ifndef XR_STDLIB_NET_IO_H
#define XR_STDLIB_NET_IO_H

#include "../../src/base/xdefs.h"
#include "tls.h"
#include "xneterror.h"
#include "../../src/coro/xnetpoll.h"
#include <stddef.h>
#include <stdbool.h>
// os_net.h provides struct iovec and writev on all platforms
// (POSIX: re-exports <sys/uio.h>; Windows: WSASend-based shim).
#include "../../src/os/os_net.h"

struct XrayIsolate;

/* ========== I/O Connection Context ========== */

typedef struct XrIOConn {
    int fd;                 // Socket file descriptor
    XrPollDesc *pd;         // Netpoll descriptor
    XrTlsConn *tls;         // TLS connection (optional)
    bool is_tls;            // Whether TLS enabled
    int timeout_ms;         // Default timeout (milliseconds)
    XrNetError last_error;  // Last error
    struct XrayIsolate *X;  // Owning isolate, captured at create; drives yield
} XrIOConn;

/* ========== Connection API ========== */

/*
 * Create TCP connection (coroutine-friendly).
 *
 * X:           owning isolate; needed to reach netpoll / DNS cache and
 *              to suspend the calling coroutine while connect() is
 *              still in progress.
 * host/port:   target endpoint.
 * timeout_ms:  connect deadline; <=0 means default (30s) is applied
 *              after the connection is created (read/write inherit it).
 *
 * Returns: connection context, NULL on failure. The caller owns the
 * returned XrIOConn and must release it via xr_io_close().
 */
XR_FUNC XrIOConn *xr_io_connect(struct XrayIsolate *X, const char *host, int port, int timeout_ms);

/*
 * Create a TLS connection wrapped over xr_io_connect().
 *
 * The caller-supplied `ctx` carries the trust store / mTLS material
 * and stays under caller ownership. It must outlive every returned
 * XrIOConn; freeing it earlier is a use-after-free.
 *
 * Returns coroutine-ready XrIOConn on success; NULL on connect /
 * handshake / verify failure (the caller never sees a half-wrapped
 * connection).
 */
XR_FUNC XrIOConn *xr_io_connect_tls_with_ctx(struct XrayIsolate *X, XrTlsContext *ctx,
                                             const char *host, int port, int timeout_ms);

// Close connection (uses conn->X to resolve netpoll). Safe to call
// on a NULL conn. Frees the conn struct itself.
XR_FUNC void xr_io_close(XrIOConn *conn);

/* ========== Read/Write API ==========
 *
 * Both read and write yield the calling coroutine via conn->X when
 * the underlying fd would block. Returning -1 means a hard failure
 * (conn->last_error carries the reason); 0 means peer closed.
 */

XR_FUNC int xr_io_read(XrIOConn *conn, void *buf, size_t len);

// Read until `len` bytes are filled, the peer closes, or an error.
// Partial reads are returned (positive count); 0/<=0 only when no
// progress was made.
XR_FUNC int xr_io_read_full(XrIOConn *conn, void *buf, size_t len);

XR_FUNC int xr_io_write(XrIOConn *conn, const void *buf, size_t len);

// Write the entire buffer or fail; partial progress is returned only
// when a later send fails after some bytes already left this side.
XR_FUNC int xr_io_write_all(XrIOConn *conn, const void *buf, size_t len);

/*
 * Scatter-gather write (plain TCP only — TLS flattens internally).
 * One writev syscall reduces the per-message copy/syscall overhead
 * the http and ws layers care about. EAGAIN on a partial drain falls
 * back to per-buffer xr_io_write_all to keep yield semantics.
 */
XR_FUNC int xr_io_writev(XrIOConn *conn, const struct iovec *iov, int iovcnt);

/* ========== Server API ========== */

/*
 * Create a listening socket. host can be NULL / "" for "all
 * interfaces". The returned fd is set non-blocking. On dual-stack
 * platforms an IPv6 bind() is preferred (V6ONLY off) so a single
 * listener accepts both IPv4 and IPv6 clients.
 *
 * Returns: listen fd on success, -1 on failure.
 */
XR_FUNC int xr_io_listen(const char *addr, int port, int backlog);

/*
 * Accept one connection (yieldable). Returns a fully initialised
 * XrIOConn whose ownership transfers to the caller. NULL on a hard
 * accept failure.
 */
XR_FUNC XrIOConn *xr_io_accept(struct XrayIsolate *X, int listen_fd);

/*
 * Accept + server-side TLS handshake. `ctx` (server context built
 * with xr_tls_context_new_server, optionally with a CA bundle for
 * mTLS) stays under caller ownership and must outlive every accepted
 * connection. NULL on any failure leaves no half-wrapped fd behind.
 */
XR_FUNC XrIOConn *xr_io_accept_tls_with_ctx(struct XrayIsolate *X, int listen_fd,
                                            XrTlsContext *ctx);

/*
 * Wrap an existing fd into an XrIOConn. The caller is responsible
 * for the fd before this call. Sets non-blocking mode and TCP_NODELAY
 * automatically.
 */
XR_FUNC XrIOConn *xr_io_conn_from_fd(struct XrayIsolate *X, int fd, int timeout_ms);

/* ========== Utility Functions ========== */

XR_FUNC void xr_io_set_timeout(XrIOConn *conn, int timeout_ms);
XR_FUNC XrNetError xr_io_get_error(XrIOConn *conn);
XR_FUNC int xr_io_set_nonblocking(int fd);

#endif  // XR_STDLIB_NET_IO_H
