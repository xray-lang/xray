/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_conn_pool.c - HTTP connection pool implementation
 *
 * KEY CONCEPT:
 *   Hash-based HTTP connection pool with LRU-style idle connection management.
 *   Thread-safe via global mutex.
 */

#include "../../src/base/xmalloc.h"
#include "../../src/os/os_time.h"
#include "http_conn_pool.h"
#include "../../src/base/xhash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void close_connection(XrHttpPooledConn *conn);

// Create new connection with DNS resolution, TCP connect, and optional TLS
// delegated to net/io so HTTP/1.1 and HTTP/2 share one coroutine I/O path.
static XrHttpPooledConn *create_connection(struct XrVMRuntime *X, XrHttpConnPool *pool,
                                           const char *host, uint16_t port, bool is_https) {
    XrIOConn *io = NULL;

#ifdef XR_ENABLE_TLS
    if (is_https) {
        if (!pool)
            return NULL;
        if (!pool->tls_ctx) {
            pool->tls_ctx = xr_tls_context_new_client();
        }
        if (!pool->tls_ctx)
            return NULL;
        io = xr_io_connect_tls_with_ctx(X, pool->tls_ctx, host, port, 30000);
    } else {
        io = xr_io_connect(X, host, port, 30000);
    }
#else
    (void) pool;
    if (is_https)
        return NULL;
    io = xr_io_connect(X, host, port, 30000);
#endif
    if (!io)
        return NULL;

    XrHttpPooledConn *conn = (XrHttpPooledConn *) xr_calloc(1, sizeof(XrHttpPooledConn));
    if (!conn) {
        xr_io_close(io);
        return NULL;
    }

    conn->io = io;
    conn->state = XR_HTTP_CONN_IN_USE;
    conn->last_used_ms = now_ms();

    return conn;
}

static void close_connection(XrHttpPooledConn *conn) {
    if (!conn)
        return;

    if (conn->io) {
        xr_io_close(conn->io);
        conn->io = NULL;
    }

    conn->state = XR_HTTP_CONN_CLOSED;
}

/* ========== Connection Pool API ========== */

XrHttpConnPool *http_conn_pool_new(void) {
    XrHttpConnPool *pool = (XrHttpConnPool *) xr_calloc(1, sizeof(XrHttpConnPool));
    if (!pool)
        return NULL;

    xr_mutex_init(&pool->lock);
    pool->initialized = true;
    pool->idle_timeout_ms = (uint64_t) XR_HTTP_POOL_MAX_IDLE_TIME * 1000;
    return pool;
}

void http_conn_pool_destroy(XrHttpConnPool *pool) {
    if (!pool || !pool->initialized)
        return;

    xr_mutex_lock(&pool->lock);

    // Close all connections. Each XrIOConn carries the owning isolate
    // needed to detach from netpoll.
    for (int i = 0; i < XR_HTTP_POOL_MAX_HOSTS; i++) {
        XrHttpHostPool *hp = pool->buckets[i];
        while (hp) {
            XrHttpHostPool *next_hp = hp->next;

            XrHttpPooledConn *conn = hp->conns;
            while (conn) {
                XrHttpPooledConn *next_conn = conn->next;
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

    pool->initialized = false;

    xr_mutex_unlock(&pool->lock);
    xr_mutex_destroy(&pool->lock);
}

XrHttpPooledConn *http_conn_pool_get(struct XrVMRuntime *X, XrHttpConnPool *pool, const char *host,
                                     uint16_t port, bool is_https) {
    if (!pool || !pool->initialized || !host)
        return NULL;

    char key[XR_HTTP_POOL_HOST_KEY_LEN];
    make_host_key(key, sizeof(key), host, port, is_https);
    uint32_t bucket = hash_string(key) % XR_HTTP_POOL_MAX_HOSTS;

    xr_mutex_lock(&pool->lock);

    // Find host pool
    XrHttpHostPool *hp = pool->buckets[bucket];
    while (hp && strcmp(hp->key, key) != 0) {
        hp = hp->next;
    }

    XrHttpPooledConn *result = NULL;

    if (hp) {
        /* Get first idle connection (no liveness check — lazy detection).
         * Skipping recv(MSG_PEEK) saves ~1μs per request. If the peer
         * has closed, the caller will get an error on first read/write
         * and can retry with a fresh connection. */
        XrHttpPooledConn **pp = &hp->conns;
        uint64_t now = now_ms();
        while (*pp) {
            XrHttpPooledConn *conn = *pp;
            if (conn->state == XR_HTTP_CONN_IDLE) {
                // Skip obviously expired (monotonic, no syscall)
                if (now - conn->last_used_ms > pool->idle_timeout_ms) {
                    *pp = conn->next;
                    close_connection(conn);
                    xr_free(conn);
                    hp->conn_count--;
                    hp->idle_count--;
                    continue;
                }
                *pp = conn->next;
                conn->next = NULL;
                conn->state = XR_HTTP_CONN_IN_USE;
                conn->last_used_ms = now;
                hp->conn_count--;
                hp->idle_count--;
                result = conn;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    xr_mutex_unlock(&pool->lock);

    // No idle connection, create new one
    if (!result) {
        result = create_connection(X, pool, host, port, is_https);
    }

    return result;
}

void http_conn_pool_put(XrHttpConnPool *pool, XrHttpPooledConn *conn, const char *host,
                        uint16_t port, bool is_https, bool keep_alive) {
    if (!pool || !pool->initialized || !conn)
        return;

    // Not keeping alive, close directly
    if (!keep_alive) {
        close_connection(conn);
        xr_free(conn);
        return;
    }

    char key[XR_HTTP_POOL_HOST_KEY_LEN];
    make_host_key(key, sizeof(key), host, port, is_https);
    uint32_t bucket = hash_string(key) % XR_HTTP_POOL_MAX_HOSTS;

    xr_mutex_lock(&pool->lock);

    // Find or create host pool
    XrHttpHostPool *hp = pool->buckets[bucket];
    while (hp && strcmp(hp->key, key) != 0) {
        hp = hp->next;
    }

    if (!hp) {
        // Create new host pool
        hp = (XrHttpHostPool *) xr_calloc(1, sizeof(XrHttpHostPool));
        if (!hp) {
            xr_mutex_unlock(&pool->lock);
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
    if (hp->idle_count >= XR_HTTP_POOL_MAX_CONNS_PER_HOST) {
        xr_mutex_unlock(&pool->lock);
        close_connection(conn);
        xr_free(conn);
        return;
    }

    // Return connection to pool
    conn->state = XR_HTTP_CONN_IDLE;
    conn->last_used_ms = now_ms();
    conn->next = hp->conns;
    hp->conns = conn;
    hp->conn_count++;
    hp->idle_count++;

    xr_mutex_unlock(&pool->lock);
}

void http_conn_pool_close(XrHttpPooledConn *conn) {
    if (!conn)
        return;
    close_connection(conn);
    xr_free(conn);
}

int http_conn_pool_evict_idle(XrHttpConnPool *pool) {
    if (!pool || !pool->initialized)
        return 0;

    uint64_t now = now_ms();
    int evicted = 0;

    xr_mutex_lock(&pool->lock);

    for (int i = 0; i < XR_HTTP_POOL_MAX_HOSTS; i++) {
        XrHttpHostPool *hp = pool->buckets[i];
        while (hp) {
            XrHttpPooledConn **pp = &hp->conns;
            while (*pp) {
                XrHttpPooledConn *conn = *pp;
                if (conn->state == XR_HTTP_CONN_IDLE &&
                    now - conn->last_used_ms > pool->idle_timeout_ms) {
                    *pp = conn->next;
                    close_connection(conn);
                    xr_free(conn);
                    hp->conn_count--;
                    hp->idle_count--;
                    evicted++;
                } else {
                    pp = &(*pp)->next;
                }
            }
            hp = hp->next;
        }
    }

    xr_mutex_unlock(&pool->lock);
    return evicted;
}

/* ========== Connection Read/Write Helpers ========== */

int http_pooled_conn_read(XrHttpPooledConn *conn, void *buf, size_t len) {
    if (!conn || !conn->io || !buf || len == 0)
        return -1;

    return xr_io_read(conn->io, buf, len);
}

int http_pooled_conn_write(XrHttpPooledConn *conn, const void *buf, size_t len) {
    if (!conn || !conn->io || !buf || len == 0)
        return -1;

    return xr_io_write(conn->io, buf, len);
}
