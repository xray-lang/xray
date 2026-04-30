/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * io.c - Coroutine-friendly I/O layer (script-binding side)
 *
 * KEY CONCEPT:
 *   netpoll, async pool and DNS cache are owned by XrRuntime
 *   (resolved via the captured XrayIsolate at create time). This file
 *   only manages the per-connection state (fd, poll desc, TLS, last
 *   error) and routes read/write/connect/accept through xsocket and
 *   the TLS layer.
 *
 *   Every public entry point either takes an XrayIsolate* directly or
 *   reads it from the conn it operates on. There is no thread-local
 *   fallback — that ambiguity used to hide concurrency bugs and
 *   pinned IO semantics to thread layout.
 */

#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/os/os_time.h"
#include "io.h"
#include "../../src/io/xdns.h"
#include "../../src/coro/xworker.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/os/os_net.h"
#include <stdlib.h>
#include <string.h>

/* ========== External: coroutine-safe socket API ========== */

extern int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);
extern int xr_socket_write(struct XrayIsolate *X, int fd, const char *buf, size_t len);
extern int xr_socket_accept(struct XrayIsolate *X, int listen_fd);

/* ========== Utilities ========== */

int xr_io_set_nonblocking(int fd) {
    return xr_socket_set_nonblocking(fd);
}

static int set_tcp_nodelay(int fd) {
    xr_socket_set_nodelay(fd, true);
#ifdef TCP_NOTSENT_LOWAT
    /* Reduce tail latency: wake writable event when <= 16KB unsent.
     * Default kernel value is often ~128KB, causing write stalls. */
    int lowat = 16384;
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
    return 0;
}

static int64_t get_deadline_ns(int timeout_ms) {
    if (timeout_ms <= 0)
        return 0;
    return (int64_t) xr_time_monotonic_ns() + (int64_t) timeout_ms * 1000000LL;
}

static XrNetpoll *netpoll_for(struct XrayIsolate *X) {
    if (!X || !X->vm.runtime)
        return NULL;
    return &((XrRuntime *) X->vm.runtime)->netpoll;
}

/* ========== Connection API ========== */

XrIOConn *xr_io_connect(struct XrayIsolate *X, const char *host, int port, int timeout_ms) {
    XR_DCHECK(X != NULL, "io_connect: NULL isolate");
    XrNetpoll *np = netpoll_for(X);
    if (!np)
        return NULL;

    // DNS resolution (dual-stack)
    XrSockAddr addr;
    if (!xr_dns_resolve(X, host, &addr, XR_AF_UNSPEC))
        return NULL;

    int fd = socket(addr.family, SOCK_STREAM, 0);
    if (fd < 0)
        return NULL;

    xr_io_set_nonblocking(fd);
    set_tcp_nodelay(fd);

    struct sockaddr *sa;
    socklen_t sa_len;
    if (addr.family == AF_INET) {
        addr.addr.v4.sin_port = htons(port);
        sa = (struct sockaddr *) &addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        addr.addr.v6.sin6_port = htons(port);
        sa = (struct sockaddr *) &addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }

    int ret = connect(fd, sa, sa_len);
    if (ret < 0 && xr_get_socket_error() != XR_EINPROGRESS) {
        xr_closesocket(fd);
        return NULL;
    }

    XrPollDesc *pd = xr_netpoll_open(np, fd);
    if (!pd) {
        xr_closesocket(fd);
        return NULL;
    }

    if (ret < 0) {
        if (timeout_ms > 0) {
            int64_t deadline = get_deadline_ns(timeout_ms);
            xr_netpoll_set_deadline(np, pd, deadline, XR_POLL_WRITE, NULL);
        }

        int wait_ret = xr_netpoll_wait(np, pd, XR_POLL_WRITE, X);
        if (wait_ret != XR_POLL_OK) {
            xr_netpoll_close(np, pd);
            xr_closesocket(fd);
            return NULL;
        }

        int error = xr_socket_get_error(fd);
        if (error != 0) {
            xr_netpoll_close(np, pd);
            xr_closesocket(fd);
            return NULL;
        }
    }

    XrIOConn *conn = (XrIOConn *) xr_calloc(1, sizeof(XrIOConn));
    if (!conn) {
        xr_netpoll_close(np, pd);
        xr_closesocket(fd);
        return NULL;
    }
    conn->fd = fd;
    conn->pd = pd;
    conn->is_tls = false;
    conn->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    conn->last_error = XR_NERR_OK;
    conn->X = X;

    return conn;
}

XrIOConn *xr_io_connect_tls_with_ctx(struct XrayIsolate *X, XrTlsContext *ctx, const char *host,
                                     int port, int timeout_ms) {
    if (!ctx)
        return NULL;

    XrIOConn *conn = xr_io_connect(X, host, port, timeout_ms);
    if (!conn)
        return NULL;

    // The caller owns `ctx` and must keep it alive while the
    // connection is open.
    conn->tls = xr_tls_conn_new(ctx, conn->fd);
    if (!conn->tls) {
        xr_io_close(conn);
        return NULL;
    }

    xr_tls_conn_set_hostname(conn->tls, host);

    XrTlsError tls_err = xr_tls_conn_handshake_client(X, conn->tls);
    if (tls_err != XR_TLS_OK) {
        conn->last_error = XR_NERR_TLS;
        xr_io_close(conn);
        return NULL;
    }

    conn->is_tls = true;
    return conn;
}

void xr_io_close(XrIOConn *conn) {
    if (!conn)
        return;

    if (conn->tls)
        xr_tls_conn_free(conn->tls);

    if (conn->pd) {
        XrNetpoll *np = netpoll_for(conn->X);
        if (np)
            xr_netpoll_close(np, conn->pd);
    }

    if (conn->fd >= 0)
        xr_closesocket(conn->fd);

    xr_free(conn);
}

/* ========== Read/Write API ========== */

int xr_io_read(XrIOConn *conn, void *buf, size_t len) {
    if (!conn || !buf || len == 0)
        return -1;

    int n;
    if (conn->is_tls) {
        n = xr_tls_conn_read(conn->X, conn->tls, buf, len);
    } else {
        n = xr_socket_read(conn->X, conn->fd, buf, len);
    }

    if (n < 0) {
        conn->last_error = XR_NERR_READ;
    } else if (n == 0) {
        conn->last_error = XR_NERR_CLOSED;
    }
    return n;
}

int xr_io_read_full(XrIOConn *conn, void *buf, size_t len) {
    size_t total = 0;
    char *p = (char *) buf;
    while (total < len) {
        int n = xr_io_read(conn, p + total, len - total);
        if (n <= 0)
            return total > 0 ? (int) total : n;
        total += n;
    }
    return (int) total;
}

int xr_io_write(XrIOConn *conn, const void *buf, size_t len) {
    if (!conn || !buf || len == 0)
        return -1;

    int n;
    if (conn->is_tls) {
        n = xr_tls_conn_write(conn->X, conn->tls, buf, len);
    } else {
        n = xr_socket_write(conn->X, conn->fd, buf, len);
    }

    if (n < 0)
        conn->last_error = XR_NERR_WRITE;
    return n;
}

int xr_io_write_all(XrIOConn *conn, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = (const char *) buf;
    while (total < len) {
        int n = xr_io_write(conn, p + total, len - total);
        if (n <= 0)
            return total > 0 ? (int) total : n;
        total += n;
    }
    return (int) total;
}

int xr_io_writev(XrIOConn *conn, const struct iovec *iov, int iovcnt) {
    if (!conn || !iov || iovcnt <= 0)
        return -1;
    if (conn->is_tls) {
        // TLS does not support writev; flatten to sequential writes
        int total = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (iov[i].iov_len == 0)
                continue;
            int n = xr_io_write_all(conn, iov[i].iov_base, iov[i].iov_len);
            if (n < 0)
                return total > 0 ? total : -1;
            total += n;
        }
        return total;
    }

    // Plain TCP: writev with a per-segment retry that yields on EAGAIN
    // by falling back to the coroutine-friendly per-buffer write.
    struct iovec local[16];
    struct iovec *vecs = local;
    if (iovcnt > 16) {
        vecs = (struct iovec *) xr_malloc(sizeof(struct iovec) * iovcnt);
        if (!vecs)
            return -1;
    }
    memcpy(vecs, iov, sizeof(struct iovec) * iovcnt);

    int total = 0;
    int cur = 0;

    while (cur < iovcnt) {
        ssize_t n = writev(conn->fd, &vecs[cur], iovcnt - cur);
        if (n > 0) {
            total += (int) n;
            size_t remain = (size_t) n;
            while (cur < iovcnt && remain >= vecs[cur].iov_len) {
                remain -= vecs[cur].iov_len;
                cur++;
            }
            if (cur < iovcnt && remain > 0) {
                vecs[cur].iov_base = (char *) vecs[cur].iov_base + remain;
                vecs[cur].iov_len -= remain;
            }
        } else if (n == 0) {
            break;
        } else {
            if (xr_socket_err_is_again(xr_get_socket_error())) {
                for (int i = cur; i < iovcnt; i++) {
                    if (vecs[i].iov_len == 0)
                        continue;
                    int w = xr_io_write_all(conn, vecs[i].iov_base, vecs[i].iov_len);
                    if (w < 0) {
                        if (vecs != local)
                            xr_free(vecs);
                        return total > 0 ? total : -1;
                    }
                    total += w;
                }
                break;
            }
            conn->last_error = XR_NERR_WRITE;
            if (vecs != local)
                xr_free(vecs);
            return total > 0 ? total : -1;
        }
    }

    if (vecs != local)
        xr_free(vecs);
    return total;
}

/* ========== Server API ========== */

int xr_io_listen(const char *addr, int port, int backlog) {
    bool force_ipv4 = false;
    if (addr && addr[0] != '\0') {
        struct in_addr tmp;
        if (inet_pton(AF_INET, addr, &tmp) == 1 && !strchr(addr, ':'))
            force_ipv4 = true;
    }

    if (!force_ipv4) {
        // Prefer IPv6 dual-stack — accepts both families on a single fd.
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd >= 0) {
            xr_socket_set_reuseaddr(fd, true);
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *) &v6only, sizeof(v6only));

            struct sockaddr_in6 sa6;
            memset(&sa6, 0, sizeof(sa6));
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port = htons(port);

            if (addr && addr[0] != '\0') {
                inet_pton(AF_INET6, addr, &sa6.sin6_addr);
            } else {
                sa6.sin6_addr = in6addr_any;
            }

            if (bind(fd, (struct sockaddr *) &sa6, sizeof(sa6)) == 0 && listen(fd, backlog) == 0) {
                xr_io_set_nonblocking(fd);
                return fd;
            }
            xr_closesocket(fd);
        }
    }

    // IPv4 fallback (or explicit IPv4 address)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    xr_socket_set_reuseaddr(fd, true);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (addr && addr[0] != '\0') {
        inet_pton(AF_INET, addr, &sa.sin_addr);
    } else {
        sa.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
        xr_closesocket(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        xr_closesocket(fd);
        return -1;
    }

    xr_io_set_nonblocking(fd);
    return fd;
}

XrIOConn *xr_io_accept(struct XrayIsolate *X, int listen_fd) {
    XR_DCHECK(X != NULL, "io_accept: NULL isolate");
    int client_fd = xr_socket_accept(X, listen_fd);
    if (client_fd < 0)
        return NULL;

    XrIOConn *conn = (XrIOConn *) xr_calloc(1, sizeof(XrIOConn));
    if (!conn) {
        xr_closesocket(client_fd);
        return NULL;
    }
    conn->fd = client_fd;
    conn->pd = NULL;  // Managed by xsocket internally
    conn->is_tls = false;
    conn->timeout_ms = 30000;
    conn->last_error = XR_NERR_OK;
    conn->X = X;
    return conn;
}

XrIOConn *xr_io_accept_tls_with_ctx(struct XrayIsolate *X, int listen_fd, XrTlsContext *ctx) {
    if (!ctx)
        return NULL;

    XrIOConn *conn = xr_io_accept(X, listen_fd);
    if (!conn)
        return NULL;

    conn->tls = xr_tls_conn_new(ctx, conn->fd);
    if (!conn->tls) {
        xr_io_close(conn);
        return NULL;
    }

    // Server-side SNI is driven by OpenSSL's callback if `ctx` was
    // configured with one; we only own the handshake driver loop.
    XrTlsError tls_err = xr_tls_conn_handshake_server(X, conn->tls);
    if (tls_err != XR_TLS_OK) {
        conn->last_error = XR_NERR_TLS;
        xr_io_close(conn);
        return NULL;
    }

    conn->is_tls = true;
    return conn;
}

XrIOConn *xr_io_conn_from_fd(struct XrayIsolate *X, int fd, int timeout_ms) {
    if (fd < 0)
        return NULL;

    xr_io_set_nonblocking(fd);
    set_tcp_nodelay(fd);

    XrIOConn *conn = (XrIOConn *) xr_calloc(1, sizeof(XrIOConn));
    if (!conn)
        return NULL;
    conn->fd = fd;
    conn->pd = NULL;
    conn->tls = NULL;
    conn->is_tls = false;
    conn->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    conn->last_error = XR_NERR_OK;
    conn->X = X;
    return conn;
}

/* ========== Utility Functions ========== */

void xr_io_set_timeout(XrIOConn *conn, int timeout_ms) {
    if (conn)
        conn->timeout_ms = timeout_ms;
}

XrNetError xr_io_get_error(XrIOConn *conn) {
    return conn ? conn->last_error : XR_NERR_CLOSED;
}
