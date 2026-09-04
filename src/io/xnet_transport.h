/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnet_transport.h - Coroutine-friendly network transport provider
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

#ifndef XR_IO_XNET_TRANSPORT_H
#define XR_IO_XNET_TRANSPORT_H

#include "../base/xdefs.h"
#include "xtls_provider.h"
#include "../../stdlib/net/xneterror.h"
#include "../coro/xnetpoll.h"
#include <stddef.h>
#include <stdbool.h>
// os_net.h provides struct iovec and writev on all platforms
// (POSIX: re-exports <sys/uio.h>; Windows: WSASend-based shim).
#include "../os/os_net.h"

struct XrVMRuntime;

/* ========== I/O Connection Context ========== */

typedef struct XrIOConn {
    int fd;                 // Socket file descriptor
    XrPollDesc *pd;         // Netpoll descriptor
    XrTlsConn *tls;         // TLS connection (optional)
    bool is_tls;            // Whether TLS enabled
    int timeout_ms;         // Default timeout (milliseconds)
    XrNetError last_error;  // Last error
    struct XrVMRuntime *X;  // Owning isolate, captured at create; drives yield
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
XR_FUNC XrIOConn *xr_io_connect(struct XrVMRuntime *X, const char *host, int port, int timeout_ms);

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
XR_FUNC XrIOConn *xr_io_connect_tls_with_ctx(struct XrVMRuntime *X, XrTlsContext *ctx,
                                             const char *host, int port, int timeout_ms);

// Close connection (uses conn->X to resolve netpoll). Safe to call
// on a NULL conn. Frees the conn struct itself.
XR_FUNC void xr_io_close(XrIOConn *conn);

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
 * Wrap an existing fd into an XrIOConn. The caller is responsible
 * for the fd before this call. Sets non-blocking mode and TCP_NODELAY
 * automatically.
 */
XR_FUNC XrIOConn *xr_io_conn_from_fd(struct XrVMRuntime *X, int fd, int timeout_ms);

/* ========== Utility Functions ========== */

XR_FUNC void xr_io_set_timeout(XrIOConn *conn, int timeout_ms);
XR_FUNC int xr_io_set_nonblocking(int fd);

#endif  // XR_IO_XNET_TRANSPORT_H
