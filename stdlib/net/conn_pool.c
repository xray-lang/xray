/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * conn_pool.c - HTTP connection pool implementation
 *
 * KEY CONCEPT:
 *   Hash-based connection pool with LRU-style idle connection management.
 *   Thread-safe via global mutex.
 */

#include "../../src/base/xmalloc.h"
#include "../../src/base/xtime.h"
#include "conn_pool.h"
#include "dns.h"
#include "io.h"
#include "../../src/base/xhash.h"
#include "../../src/coro/xnetpoll.h"
#include "../../src/coro/xworker.h"     // For XrRuntime
#include "../../src/vm/xvm_internal.h"  // For XrayIsolate->vm.runtime
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>

// Coroutine-safe fd I/O primitives (implemented in src/coro/xsocket.c).
// When the caller is running on a coroutine, these yield through the
// netpoll / pthread_cond_wait machinery instead of blocking the worker
// in recv/send. Return values match the usual short-read/short-write
// conventions plus -1 on error.
extern int xr_socket_read(struct XrayIsolate *X, int fd, char *buf, size_t len);
extern int xr_socket_write(struct XrayIsolate *X, int fd, const char *buf, size_t len);

// Monotonic milliseconds (no syscall on most platforms via vDSO)
static uint64_t now_ms(void) {
    return xr_time_monotonic_ms();
}

/* ========== Helper Functions ========== */

// Generate host key: "host:port:https"
static void make_host_key(char *key, size_t key_len, const char *host, uint16_t port,
                          bool is_https) {
    snprintf(key, key_len, "%s:%d:%d", host, port, is_https ? 1 : 0);
}

static uint32_t hash_string(const char *str) {
    return xr_hash_bytes(str, strlen(str));
}

// Perform a non-blocking TCP connect that cooperates with the coroutine
// scheduler. When called from a coroutine (X != NULL) and the fd blocks
// on EINPROGRESS, we suspend on runtime->netpoll via xr_netpoll_block so
// the worker can service other coroutines until the socket is writable.
// Returns 0 on success, -1 on failure (socket closed by caller on error).
static int coro_tcp_connect(int fd, const struct sockaddr *sa, socklen_t sa_len,
                            struct XrayIsolate *X) {
    int ret = connect(fd, sa, sa_len);
    if (ret == 0)
        return 0;
    if (errno != EINPROGRESS && errno != EWOULDBLOCK)
        return -1;

    // Caller expected us to be on a coroutine but we have no runtime: fall
    // back to a blocking poll loop on SO_ERROR. This keeps the pool usable
    // from non-VM contexts (tests, CLI tools) without crashing.
    XrRuntime *runtime = X ? (XrRuntime *) X->vm.runtime : NULL;
    if (!runtime) {
        // 30 s upper bound so a black-holed peer can't hang the
        // caller forever in non-VM contexts (CLI, unit tests).
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        if (poll(&pfd, 1, 30000) <= 0)
            return -1;
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            return -1;
        }
        return 0;
    }

    XrPollDesc *pd = xr_netpoll_open(&runtime->netpoll, fd);
    if (!pd)
        return -1;

    // xr_netpoll_block sleeps the current coroutine until the fd is
    // writable (connect completion) or the pd is closed. On spurious
    // wake it returns true but the SO_ERROR check below catches any real
    // failure, so a single check after resume is sufficient.
    if (!xr_netpoll_block(pd, XR_POLL_WRITE, X))
        return -1;

    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
        return -1;
    }
    return 0;
}

// Create new connection with DNS resolution and optional TLS. Made fd
// non-blocking from creation so all subsequent pooled_conn_read/write calls
// can go through xr_socket_read/write and yield the coroutine cleanly.
static XrPooledConn *create_connection(XrConnPool *pool, const char *host, uint16_t port,
                                       bool is_https) {
#ifndef XR_ENABLE_TLS
    // When TLS is compiled out the pool handle is only needed for the
    // tls_ctx lookup below; silence -Wunused-parameter without touching
    // the prototype (which is shared with TLS-enabled builds).
    (void) pool;
#endif
    // Resolve all addresses (dual-stack, IPv6/IPv4 interleaved) and try
    // them in order, bailing out on first success. This is a coarse
    // Happy-Eyeballs approximation — we do not fire parallel connects.
    // True HE v2 (RFC 8305) needs a multi-fd coroutine-wait primitive
    // we do not yet have. Serial failover alone already rescues the
    // common "one v6 route is black-holed" case.
    //
    // Note: on cache miss this still performs a blocking getaddrinfo;
    // an async DNS path is not yet implemented.
    XrSockAddr addrs[XR_DNS_MAX_ADDRS];
    int n = xr_dns_resolve_all(host, addrs, XR_DNS_MAX_ADDRS, XR_AF_UNSPEC);
    if (n <= 0)
        return NULL;

    struct XrayIsolate *X = xr_io_get_isolate();
    int fd = -1;
    for (int i = 0; i < n; i++) {
        int try_fd = socket(addrs[i].family, SOCK_STREAM, 0);
        if (try_fd < 0)
            continue;
        xr_io_set_nonblocking(try_fd);
        int flag = 1;
        setsockopt(try_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        struct sockaddr *sa;
        socklen_t sa_len;
        if (addrs[i].family == AF_INET) {
            addrs[i].addr.v4.sin_port = htons(port);
            sa = (struct sockaddr *) &addrs[i].addr.v4;
            sa_len = sizeof(struct sockaddr_in);
        } else {
            addrs[i].addr.v6.sin6_port = htons(port);
            sa = (struct sockaddr *) &addrs[i].addr.v6;
            sa_len = sizeof(struct sockaddr_in6);
        }

        if (coro_tcp_connect(try_fd, sa, sa_len, X) == 0) {
            fd = try_fd;
            break;
        }
        // Failed: close this fd (also releases its netpoll pd since
        // coro_tcp_connect registered one) and try the next address.
        if (X && X->vm.runtime) {
            XrRuntime *runtime = (XrRuntime *) X->vm.runtime;
            XrPollDesc *pd = xr_fdmap_get(&runtime->netpoll, try_fd);
            if (pd && !atomic_load(&pd->closing)) {
                xr_netpoll_close(&runtime->netpoll, pd);
            }
        }
        close(try_fd);
    }
    if (fd < 0)
        return NULL;

    // Create pooled connection
    XrPooledConn *conn = (XrPooledConn *) xr_calloc(1, sizeof(XrPooledConn));
    if (!conn) {
        close(fd);
        return NULL;
    }

    conn->fd = fd;
    conn->state = XR_CONN_IN_USE;
    conn->created_ms = now_ms();
    conn->last_used_ms = conn->created_ms;
    conn->is_https = is_https;
    conn->tls_conn = NULL;

#ifdef XR_ENABLE_TLS
    // HTTPS: TLS handshake (coroutine-friendly).
    //
    // Use the non-blocking xr_tls_conn_handshake_try() in a loop.  On
    // WANT_READ / WANT_WRITE we suspend the current coroutine via
    // xr_netpoll_block so the worker can service other goroutines
    // while the TCP round-trips complete. Without a runtime (CLI /
    // tests) we fall back to poll() with a bounded timeout so the
    // caller never busy-loops or hangs indefinitely.
    //
    // All failure paths go through close_connection() which properly
    // deregisters the fd from netpoll before close(), avoiding a stale
    // pollDesc that would corrupt future fd reuse.
    if (is_https && pool) {
        if (!pool->tls_ctx) {
            pool->tls_ctx = xr_tls_context_new_client();
        }

        if (pool->tls_ctx) {
            conn->tls_conn = xr_tls_conn_new(pool->tls_ctx, fd);
            if (conn->tls_conn) {
                xr_tls_conn_set_hostname(conn->tls_conn, host);

                // Non-blocking handshake loop
                bool hs_ok = false;
                for (;;) {
                    int hs = xr_tls_conn_handshake_try(conn->tls_conn);
                    if (hs == 0) {
                        hs_ok = true;
                        break;
                    }  // Success
                    if (hs < 0) {
                        break;
                    }  // Fatal error

                    // hs == 1 → WANT_READ, hs == 2 → WANT_WRITE
                    XrPollMode mode = (hs == 1) ? XR_POLL_READ : XR_POLL_WRITE;
                    XrRuntime *rt = X ? (XrRuntime *) X->vm.runtime : NULL;
                    if (rt) {
                        XrPollDesc *pd = xr_netpoll_open(&rt->netpoll, fd);
                        if (!pd || !xr_netpoll_block(pd, mode, X)) {
                            break;  // Netpoll error → abort
                        }
                    } else {
                        // Non-VM fallback: bounded poll so we don't spin
                        struct pollfd pfd = {
                            .fd = fd,
                            .events = (short) ((mode == XR_POLL_READ) ? POLLIN : POLLOUT)};
                        if (poll(&pfd, 1, 30000) <= 0) {
                            break;
                        }
                    }
                }

                if (!hs_ok) {
                    close_connection(conn);
                    xr_free(conn);
                    return NULL;
                }
            }
        }
    }
#endif

    return conn;
}

// Close connection and cleanup TLS. Also deregisters the fd from the
// runtime's netpoll (if the fd ever went through coro_tcp_connect or
// xr_socket_read/write) so that no stale pollDesc survives the close.
static void close_connection(XrPooledConn *conn) {
    if (!conn)
        return;

#ifdef XR_ENABLE_TLS
    if (conn->tls_conn) {
        xr_tls_conn_close(conn->tls_conn);
        xr_tls_conn_free(conn->tls_conn);
        conn->tls_conn = NULL;
    }
#endif

    if (conn->fd >= 0) {
        // Detach from netpoll before close() so we don't leak a
        // pollDesc keyed on a reused fd. xr_fdmap_get is O(1) and
        // returns NULL if the fd was never registered.
        struct XrayIsolate *X = xr_io_get_isolate();
        XrRuntime *runtime = X ? (XrRuntime *) X->vm.runtime : NULL;
        if (runtime) {
            XrPollDesc *pd = xr_fdmap_get(&runtime->netpoll, conn->fd);
            if (pd && !atomic_load(&pd->closing)) {
                xr_netpoll_close(&runtime->netpoll, pd);
            }
        }
        close(conn->fd);
        conn->fd = -1;
    }

    conn->state = XR_CONN_CLOSED;
}

/* ========== Connection Pool API ========== */

XrConnPool *xr_conn_pool_new(void) {
    XrConnPool *pool = (XrConnPool *) xr_calloc(1, sizeof(XrConnPool));
    if (!pool)
        return NULL;

    pthread_mutex_init(&pool->lock, NULL);
    pool->initialized = true;
    pool->idle_timeout_ms = (uint64_t) XR_POOL_MAX_IDLE_TIME * 1000;
    return pool;
}

void xr_conn_pool_init(XrConnPool *pool) {
    if (!pool || pool->initialized)
        return;

    memset(pool, 0, sizeof(XrConnPool));
    pthread_mutex_init(&pool->lock, NULL);
    pool->initialized = true;
    pool->idle_timeout_ms = (uint64_t) XR_POOL_MAX_IDLE_TIME * 1000;
}

void xr_conn_pool_destroy(XrConnPool *pool) {
    if (!pool || !pool->initialized)
        return;

    pthread_mutex_lock(&pool->lock);

    // Close all connections
    for (int i = 0; i < XR_POOL_MAX_HOSTS; i++) {
        XrHostPool *hp = pool->buckets[i];
        while (hp) {
            XrHostPool *next_hp = hp->next;

            XrPooledConn *conn = hp->conns;
            while (conn) {
                XrPooledConn *next_conn = conn->next;
                close_connection(conn);
                xr_free(conn);
                conn = next_conn;
            }

            xr_free(hp);
            hp = next_hp;
        }
        pool->buckets[i] = NULL;
    }

#ifdef XR_ENABLE_TLS
    if (pool->tls_ctx) {
        xr_tls_context_free(pool->tls_ctx);
        pool->tls_ctx = NULL;
    }
#endif

    pool->total_conns = 0;
    pool->initialized = false;

    pthread_mutex_unlock(&pool->lock);
    pthread_mutex_destroy(&pool->lock);
}

XrPooledConn *xr_conn_pool_get(XrConnPool *pool, const char *host, uint16_t port, bool is_https) {
    if (!pool || !pool->initialized || !host)
        return NULL;

    char key[XR_POOL_HOST_KEY_LEN];
    make_host_key(key, sizeof(key), host, port, is_https);
    uint32_t bucket = hash_string(key) % XR_POOL_MAX_HOSTS;

    pthread_mutex_lock(&pool->lock);

    // Find host pool
    XrHostPool *hp = pool->buckets[bucket];
    while (hp && strcmp(hp->key, key) != 0) {
        hp = hp->next;
    }

    XrPooledConn *result = NULL;

    if (hp) {
        /* Get first idle connection (no liveness check — lazy detection).
         * Skipping recv(MSG_PEEK) saves ~1μs per request. If the peer
         * has closed, the caller will get an error on first read/write
         * and can retry with a fresh connection. */
        XrPooledConn **pp = &hp->conns;
        uint64_t now = now_ms();
        while (*pp) {
            XrPooledConn *conn = *pp;
            if (conn->state == XR_CONN_IDLE) {
                // Skip obviously expired (monotonic, no syscall)
                if (now - conn->last_used_ms > pool->idle_timeout_ms) {
                    *pp = conn->next;
                    close_connection(conn);
                    xr_free(conn);
                    hp->conn_count--;
                    hp->idle_count--;
                    pool->total_conns--;
                    continue;
                }
                conn->state = XR_CONN_IN_USE;
                conn->last_used_ms = now;
                hp->idle_count--;
                result = conn;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    pthread_mutex_unlock(&pool->lock);

    // No idle connection, create new one
    if (!result) {
        result = create_connection(pool, host, port, is_https);
    }

    return result;
}

void xr_conn_pool_put(XrConnPool *pool, XrPooledConn *conn, const char *host, uint16_t port,
                      bool is_https, bool keep_alive) {
    if (!pool || !pool->initialized || !conn)
        return;

    // Not keeping alive, close directly
    if (!keep_alive) {
        close_connection(conn);
        xr_free(conn);
        return;
    }

    char key[XR_POOL_HOST_KEY_LEN];
    make_host_key(key, sizeof(key), host, port, is_https);
    uint32_t bucket = hash_string(key) % XR_POOL_MAX_HOSTS;

    pthread_mutex_lock(&pool->lock);

    // Find or create host pool
    XrHostPool *hp = pool->buckets[bucket];
    while (hp && strcmp(hp->key, key) != 0) {
        hp = hp->next;
    }

    if (!hp) {
        // Create new host pool
        hp = (XrHostPool *) xr_calloc(1, sizeof(XrHostPool));
        if (!hp) {
            pthread_mutex_unlock(&pool->lock);
            close_connection(conn);
            xr_free(conn);
            return;
        }

        strncpy(hp->key, key, sizeof(hp->key) - 1);
        strncpy(hp->host, host, sizeof(hp->host) - 1);
        hp->port = port;
        hp->is_https = is_https;

        hp->next = pool->buckets[bucket];
        pool->buckets[bucket] = hp;
    }

    // Check connection limit
    if (hp->idle_count >= XR_POOL_MAX_CONNS_PER_HOST) {
        pthread_mutex_unlock(&pool->lock);
        close_connection(conn);
        xr_free(conn);
        return;
    }

    // Return connection to pool
    conn->state = XR_CONN_IDLE;
    conn->last_used_ms = now_ms();
    conn->next = hp->conns;
    hp->conns = conn;
    hp->conn_count++;
    hp->idle_count++;
    pool->total_conns++;

    pthread_mutex_unlock(&pool->lock);
}

void xr_conn_pool_close(XrConnPool *pool, XrPooledConn *conn) {
    (void) pool;
    if (!conn)
        return;
    close_connection(conn);
    xr_free(conn);
}

int xr_conn_pool_evict_idle(XrConnPool *pool) {
    if (!pool || !pool->initialized)
        return 0;

    uint64_t now = now_ms();
    int evicted = 0;

    pthread_mutex_lock(&pool->lock);

    for (int i = 0; i < XR_POOL_MAX_HOSTS; i++) {
        XrHostPool *hp = pool->buckets[i];
        while (hp) {
            XrPooledConn **pp = &hp->conns;
            while (*pp) {
                XrPooledConn *conn = *pp;
                if (conn->state == XR_CONN_IDLE &&
                    now - conn->last_used_ms > pool->idle_timeout_ms) {
                    *pp = conn->next;
                    close_connection(conn);
                    xr_free(conn);
                    hp->conn_count--;
                    hp->idle_count--;
                    pool->total_conns--;
                    evicted++;
                } else {
                    pp = &(*pp)->next;
                }
            }
            hp = hp->next;
        }
    }

    pthread_mutex_unlock(&pool->lock);
    return evicted;
}

void xr_conn_pool_cleanup(XrConnPool *pool) {
    xr_conn_pool_evict_idle(pool);
}

void xr_conn_pool_stats(XrConnPool *pool, int *total, int *idle) {
    if (!pool || !pool->initialized) {
        if (total)
            *total = 0;
        if (idle)
            *idle = 0;
        return;
    }

    pthread_mutex_lock(&pool->lock);

    int t = 0, i = 0;
    for (int b = 0; b < XR_POOL_MAX_HOSTS; b++) {
        XrHostPool *hp = pool->buckets[b];
        while (hp) {
            t += hp->conn_count;
            i += hp->idle_count;
            hp = hp->next;
        }
    }

    if (total)
        *total = t;
    if (idle)
        *idle = i;

    pthread_mutex_unlock(&pool->lock);
}

/* ========== Connection Read/Write Helpers ========== */

int xr_pooled_conn_read(XrPooledConn *conn, void *buf, size_t len) {
    if (!conn || conn->fd < 0 || !buf || len == 0)
        return -1;

#ifdef XR_ENABLE_TLS
    // TLS read path already yields via xr_socket_read internally.
    if (conn->tls_conn) {
        return xr_tls_conn_read(conn->tls_conn, buf, len);
    }
#endif

    // Plain TCP: on a coroutine, hand off to xr_socket_read which handles
    // EAGAIN by suspending on runtime->netpoll. Outside a coroutine (CLI /
    // tests) drop to a plain recv — the fd is non-blocking since
    // create_connection, so a caller without a scheduler either gets data
    // or EAGAIN immediately, which preserves the old failure semantics
    // instead of hanging a non-VM thread forever.
    struct XrayIsolate *X = xr_io_get_isolate();
    if (X) {
        return xr_socket_read(X, conn->fd, (char *) buf, len);
    }
    return (int) recv(conn->fd, buf, len, 0);
}

int xr_pooled_conn_write(XrPooledConn *conn, const void *buf, size_t len) {
    if (!conn || conn->fd < 0 || !buf || len == 0)
        return -1;

#ifdef XR_ENABLE_TLS
    // TLS write path already yields via xr_socket_write internally.
    if (conn->tls_conn) {
        return xr_tls_conn_write(conn->tls_conn, buf, len);
    }
#endif

    // Plain TCP: mirror the read path above. xr_socket_write loops until
    // all bytes are drained or an error is hit, so a single call here is
    // sufficient (no outer "keep writing" loop needed in http_client).
    struct XrayIsolate *X = xr_io_get_isolate();
    if (X) {
        return xr_socket_write(X, conn->fd, (const char *) buf, len);
    }
    return (int) send(conn->fd, buf, len, 0);
}
