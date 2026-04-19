/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * io.c - Coroutine-friendly I/O layer implementation
 *
 * KEY CONCEPT:
 *   Netpoll-based I/O with automatic yield/resume for coroutines.
 *   Integrates DNS, TLS, and TCP into unified interface.
 */

#include "io.h"
#include "dns.h"
#include "../../include/xray_platform.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef XR_PLATFORM_WINDOWS
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

/* ========== Global State ========== */

static struct {
    XrNetpoll owned_netpoll; // Owned instance (standalone mode)
    XrNetpoll *np; // Active netpoll (owned or external)
    XrTlsContext *tls_ctx;
    bool initialized;
    bool netpoll_external; // true if np points to external netpoll
} g_io;

// Thread-local: current VM instance (for coroutine scheduling)
static _Thread_local struct XrayIsolate *tls_isolate = NULL;

void xr_io_set_isolate(struct XrayIsolate *X) {
    tls_isolate = X;
}

struct XrayIsolate* xr_io_get_isolate(void) {
    return tls_isolate;
}

/* ========== Utility Functions ========== */

int xr_io_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_tcp_nodelay(int fd)
{
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#ifdef TCP_NOTSENT_LOWAT
    /* Reduce tail latency: wake writable event when <= 16KB unsent.
     * Default kernel value is often ~128KB, causing write stalls. */
    int lowat = 16384;
    setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
    return 0;
}

static int64_t get_deadline_ns(int timeout_ms)
{
    if (timeout_ms <= 0) return 0;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000000LL +
           (int64_t)tv.tv_usec * 1000LL +
           (int64_t)timeout_ms * 1000000LL;
}

/* ========== Global Initialization ========== */

void xr_io_init(void)
{
    if (g_io.initialized) return;

    memset(&g_io, 0, sizeof(g_io));

    if (xr_netpoll_init(&g_io.owned_netpoll) != 0) {
        return;
    }
    g_io.np = &g_io.owned_netpoll;
    g_io.netpoll_external = false;

    // Initialize TLS
    xr_tls_init();
    g_io.tls_ctx = xr_tls_context_new_client();

    // Initialize DNS
    xr_dns_init();

    g_io.initialized = true;
}

void xr_io_init_with_netpoll(XrNetpoll *np)
{
    if (g_io.initialized) return;
    if (!np) { xr_io_init(); return; }

    memset(&g_io, 0, sizeof(g_io));
    g_io.np = np;
    g_io.netpoll_external = true;

    xr_tls_init();
    g_io.tls_ctx = xr_tls_context_new_client();
    xr_dns_init();

    g_io.initialized = true;
}

void xr_io_shutdown(void)
{
    if (!g_io.initialized) return;

    xr_dns_shutdown();

    if (g_io.tls_ctx) {
        xr_tls_context_free(g_io.tls_ctx);
        g_io.tls_ctx = NULL;
    }

    if (!g_io.netpoll_external) {
        xr_netpoll_cleanup(&g_io.owned_netpoll);
    }
    g_io.np = NULL;
    xr_tls_cleanup();

    g_io.initialized = false;
}

XrNetpoll* xr_io_get_netpoll(void)
{
    return g_io.np;
}

/* ========== Connection API ========== */

XrIOConn* xr_io_connect(const char *host, int port, int timeout_ms)
{
    if (!g_io.initialized) {
        xr_io_init();
    }

    // DNS resolution (dual-stack)
    XrSockAddr addr;
    if (!xr_dns_resolve(host, &addr, XR_AF_UNSPEC)) {
        return NULL;
    }

    // Create socket (based on address family)
    int fd = socket(addr.family, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }

    // Set non-blocking and TCP_NODELAY
    xr_io_set_nonblocking(fd);
    set_tcp_nodelay(fd);

    // Build address and connect
    struct sockaddr *sa;
    socklen_t sa_len;

    if (addr.family == AF_INET) {
        addr.addr.v4.sin_port = htons(port);
        sa = (struct sockaddr*)&addr.addr.v4;
        sa_len = sizeof(struct sockaddr_in);
    } else {
        addr.addr.v6.sin6_port = htons(port);
        sa = (struct sockaddr*)&addr.addr.v6;
        sa_len = sizeof(struct sockaddr_in6);
    }

    // Non-blocking connect
    int ret = connect(fd, sa, sa_len);
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        return NULL;
    }

    // Register with netpoll
    XrPollDesc *pd = xr_netpoll_open(g_io.np, fd);
    if (!pd) {
        close(fd);
        return NULL;
    }

    // Wait for connection if not immediate
    if (ret < 0) {
        // Set timeout
        if (timeout_ms > 0) {
            int64_t deadline = get_deadline_ns(timeout_ms);
            xr_netpoll_set_deadline(g_io.np, pd, deadline, XR_POLL_WRITE, NULL);
        }

        // Wait for connection (coroutine yields)
        int wait_ret = xr_netpoll_wait(g_io.np, pd, XR_POLL_WRITE, tls_isolate);
        if (wait_ret != XR_POLL_OK) {
            xr_netpoll_close(g_io.np, pd);
            close(fd);
            return NULL;
        }

        // Check connection result
        int error = 0;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
            xr_netpoll_close(g_io.np, pd);
            close(fd);
            return NULL;
        }
    }

    // Create connection context
    XrIOConn *conn = (XrIOConn*)calloc(1, sizeof(XrIOConn));
    conn->fd = fd;
    conn->pd = pd;
    conn->is_tls = false;
    conn->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    conn->last_error = XR_NERR_OK;

    return conn;
}

XrIOConn* xr_io_connect_tls(const char *host, int port, int timeout_ms)
{
    // Delegate to the explicit-context variant using the module's shared
    // client context (system trust store). Keeps a single TLS handshake
    // code path to maintain.
    return xr_io_connect_tls_with_ctx(g_io.tls_ctx, host, port, timeout_ms);
}

XrIOConn* xr_io_connect_tls_with_ctx(XrTlsContext *ctx, const char *host,
                                      int port, int timeout_ms)
{
    if (!ctx) return NULL;

    // Establish TCP connection first (also initialises netpoll if needed).
    XrIOConn *conn = xr_io_connect(host, port, timeout_ms);
    if (!conn) {
        return NULL;
    }

    // Wrap the connected fd with the caller-supplied TLS context. The
    // caller owns `ctx` and must keep it alive while the connection is
    // open. g_io.tls_ctx is only used here when the caller explicitly
    // passes it in via xr_io_connect_tls.
    conn->tls = xr_tls_conn_new(ctx, conn->fd);
    if (!conn->tls) {
        xr_io_close(conn);
        return NULL;
    }

    xr_tls_conn_set_hostname(conn->tls, host);

    XrTlsError tls_err = xr_tls_conn_handshake_client(conn->tls);
    if (tls_err != XR_TLS_OK) {
        conn->last_error = XR_NERR_TLS;
        xr_io_close(conn);
        return NULL;
    }

    conn->is_tls = true;
    return conn;
}

void xr_io_close(XrIOConn *conn)
{
    if (!conn) return;

    if (conn->tls) {
        xr_tls_conn_free(conn->tls);
    }

    if (conn->pd) {
        xr_netpoll_close(g_io.np, conn->pd);
    }

    if (conn->fd >= 0) {
        close(conn->fd);
    }

    free(conn);
}

/* ========== Read/Write API ========== */

// External: coroutine-safe socket API
extern int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);
extern int xr_socket_write(struct XrayIsolate *X, int fd, const char *buf, size_t len);

int xr_io_read(XrIOConn *conn, void *buf, size_t len)
{
    if (!conn || !buf || len == 0) return -1;

    int n;
    if (conn->is_tls) {
        n = xr_tls_conn_read(conn->tls, buf, len);
    } else {
        n = xr_socket_read(tls_isolate, conn->fd, buf, len);
    }

    if (n < 0) {
        conn->last_error = XR_NERR_READ;
    } else if (n == 0) {
        conn->last_error = XR_NERR_CLOSED;
    }
    return n;
}

int xr_io_read_full(XrIOConn *conn, void *buf, size_t len)
{
    size_t total = 0;
    char *p = (char*)buf;

    while (total < len) {
        int n = xr_io_read(conn, p + total, len - total);
        if (n <= 0) {
            return total > 0 ? (int)total : n;
        }
        total += n;
    }

    return (int)total;
}

int xr_io_write(XrIOConn *conn, const void *buf, size_t len)
{
    if (!conn || !buf || len == 0) return -1;

    int n;
    if (conn->is_tls) {
        n = xr_tls_conn_write(conn->tls, buf, len);
    } else {
        n = xr_socket_write(tls_isolate, conn->fd, buf, len);
    }

    if (n < 0) {
        conn->last_error = XR_NERR_WRITE;
    }
    return n;
}

int xr_io_write_all(XrIOConn *conn, const void *buf, size_t len)
{
    size_t total = 0;
    const char *p = (const char*)buf;

    while (total < len) {
        int n = xr_io_write(conn, p + total, len - total);
        if (n <= 0) {
            return total > 0 ? (int)total : n;
        }
        total += n;
    }

    return (int)total;
}

#ifndef XR_PLATFORM_WINDOWS
int xr_io_writev(XrIOConn *conn, const struct iovec *iov, int iovcnt)
{
    if (!conn || !iov || iovcnt <= 0) return -1;
    if (conn->is_tls) {
        // TLS does not support writev; flatten to sequential writes
        int total = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (iov[i].iov_len == 0) continue;
            int n = xr_io_write_all(conn, iov[i].iov_base, iov[i].iov_len);
            if (n < 0) return total > 0 ? total : -1;
            total += n;
        }
        return total;
    }

    // Plain TCP: use writev syscall with retry loop
    // Make a mutable copy of iov for advancing pointers
    struct iovec local[16];
    struct iovec *vecs = local;
    if (iovcnt > 16) {
        vecs = (struct iovec *)malloc(sizeof(struct iovec) * iovcnt);
        if (!vecs) return -1;
    }
    memcpy(vecs, iov, sizeof(struct iovec) * iovcnt);

    int total = 0;
    int cur = 0;

    while (cur < iovcnt) {
        ssize_t n = writev(conn->fd, &vecs[cur], iovcnt - cur);
        if (n > 0) {
            total += (int)n;
            // Advance iov past written bytes
            size_t remain = (size_t)n;
            while (cur < iovcnt && remain >= vecs[cur].iov_len) {
                remain -= vecs[cur].iov_len;
                cur++;
            }
            if (cur < iovcnt && remain > 0) {
                vecs[cur].iov_base = (char *)vecs[cur].iov_base + remain;
                vecs[cur].iov_len -= remain;
            }
        } else if (n == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Yield and retry via coroutine-safe write for remaining
                for (int i = cur; i < iovcnt; i++) {
                    if (vecs[i].iov_len == 0) continue;
                    int w = xr_io_write_all(conn, vecs[i].iov_base, vecs[i].iov_len);
                    if (w < 0) { if (vecs != local) free(vecs); return total > 0 ? total : -1; }
                    total += w;
                }
                break;
            }
            conn->last_error = XR_NERR_WRITE;
            if (vecs != local) free(vecs);
            return total > 0 ? total : -1;
        }
    }

    if (vecs != local) free(vecs);
    return total;
}
#endif

/* ========== Server API ========== */

int xr_io_listen(const char *addr, int port, int backlog)
{
    if (!g_io.initialized) {
        xr_io_init();
    }

    // Detect if caller explicitly wants IPv4 (e.g. "127.0.0.1")
    bool force_ipv4 = false;
    if (addr && addr[0] != '\0') {
        struct in_addr tmp;
        if (inet_pton(AF_INET, addr, &tmp) == 1 && !strchr(addr, ':')) {
            force_ipv4 = true;
        }
    }

    if (!force_ipv4) {
        // Try IPv6 dual-stack (accepts both IPv4 and IPv6)
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd >= 0) {
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            int v6only = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

            struct sockaddr_in6 sa6;
            memset(&sa6, 0, sizeof(sa6));
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port = htons(port);

            if (addr && addr[0] != '\0') {
                inet_pton(AF_INET6, addr, &sa6.sin6_addr);
            } else {
                sa6.sin6_addr = in6addr_any;
            }

            if (bind(fd, (struct sockaddr*)&sa6, sizeof(sa6)) == 0 &&
                listen(fd, backlog) == 0) {
                xr_io_set_nonblocking(fd);
                return fd;
            }
            close(fd);
            // Fall through to IPv4
        }
    }

    // IPv4 fallback (or explicit IPv4 address)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);

    if (addr && addr[0] != '\0') {
        inet_pton(AF_INET, addr, &sa.sin_addr);
    } else {
        sa.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }

    xr_io_set_nonblocking(fd);

    return fd;
}

// External: coroutine-safe accept
extern int xr_socket_accept(struct XrayIsolate *X, int listen_fd);

XrIOConn* xr_io_accept(int listen_fd)
{
    // Use coroutine-safe xsocket API
    int client_fd = xr_socket_accept(tls_isolate, listen_fd);

    if (client_fd < 0) {
        return NULL;
    }

    // Create connection context
    XrIOConn *conn = (XrIOConn*)calloc(1, sizeof(XrIOConn));
    if (!conn) {
        close(client_fd);
        return NULL;
    }

    conn->fd = client_fd;
    conn->pd = NULL;  // Managed by xsocket internally
    conn->is_tls = false;
    conn->timeout_ms = 30000;
    conn->last_error = XR_NERR_OK;

    return conn;
}

XrIOConn* xr_io_accept_tls_with_ctx(int listen_fd, XrTlsContext *ctx)
{
    if (!ctx) return NULL;

    // Do the plaintext accept first. This also yields the coroutine if
    // no client is pending and sets the accepted fd non-blocking so the
    // TLS handshake can drive through xr_socket_read/write.
    XrIOConn *conn = xr_io_accept(listen_fd);
    if (!conn) return NULL;

    conn->tls = xr_tls_conn_new(ctx, conn->fd);
    if (!conn->tls) {
        xr_io_close(conn);
        return NULL;
    }

    // SNI on the server side is handled by OpenSSL's callback if the
    // operator configured one on `ctx`. We only drive the handshake.
    XrTlsError tls_err = xr_tls_conn_handshake_server(conn->tls);
    if (tls_err != XR_TLS_OK) {
        conn->last_error = XR_NERR_TLS;
        xr_io_close(conn);
        return NULL;
    }

    conn->is_tls = true;
    return conn;
}

XrIOConn* xr_io_conn_from_fd(int fd, int timeout_ms)
{
    if (fd < 0) return NULL;

    if (!g_io.initialized) {
        xr_io_init();
    }

    xr_io_set_nonblocking(fd);
    set_tcp_nodelay(fd);

    XrIOConn *conn = (XrIOConn*)calloc(1, sizeof(XrIOConn));
    if (!conn) return NULL;

    conn->fd = fd;
    conn->pd = NULL;
    conn->tls = NULL;
    conn->is_tls = false;
    conn->timeout_ms = timeout_ms > 0 ? timeout_ms : 30000;
    conn->last_error = XR_NERR_OK;

    return conn;
}

/* ========== Utilities ========== */

void xr_io_set_timeout(XrIOConn *conn, int timeout_ms)
{
    if (conn) {
        conn->timeout_ms = timeout_ms;
    }
}

XrNetError xr_io_get_error(XrIOConn *conn)
{
    return conn ? conn->last_error : XR_NERR_CLOSED;
}

