/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_conn_pool.h - HTTP connection pool
 *
 * KEY CONCEPT:
 *   Manages persistent TCP/TLS connections grouped by host:port:https.
 *   Supports Keep-Alive, idle timeout cleanup, and thread-safe access.
 */

#ifndef XR_STDLIB_HTTP_CONN_POOL_H
#define XR_STDLIB_HTTP_CONN_POOL_H

#include "../net/io.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../src/os/os_thread.h"

struct XrVMRuntime;

/* ========== Constants ========== */

#define XR_HTTP_POOL_MAX_CONNS_PER_HOST 6  // Max connections per host
#define XR_HTTP_POOL_MAX_IDLE_TIME 60      // Idle timeout in seconds
#define XR_HTTP_POOL_MAX_HOSTS 64          // Max number of hosts
#define XR_HTTP_POOL_HOST_KEY_LEN 256      // Max length of host key

/* ========== Connection State ========== */

typedef enum {
    XR_HTTP_CONN_IDLE = 0,
    XR_HTTP_CONN_IN_USE,
    XR_HTTP_CONN_CLOSED
} XrHttpConnState;

/* ========== Pooled Connection ========== */

typedef struct XrHttpPooledConn {
    XrIOConn *io;
    XrHttpConnState state;
    uint64_t last_used_ms;  // Monotonic timestamp (milliseconds)
    struct XrHttpPooledConn *next;
} XrHttpPooledConn;

/* ========== Host Pool ========== */

typedef struct XrHttpHostPool {
    char key[XR_HTTP_POOL_HOST_KEY_LEN];  // Format: "host:port:https"
    char host[128];
    uint16_t port;
    bool is_https;
    XrHttpPooledConn *conns;  // Connection list
    int conn_count;
    int idle_count;
    struct XrHttpHostPool *next;  // Hash collision chain
} XrHttpHostPool;

/* ========== HTTP Connection Pool ========== */

typedef struct XrHttpConnPool {
    XrHttpHostPool *buckets[XR_HTTP_POOL_MAX_HOSTS];  // Hash buckets
    xr_mutex_t lock;                                  // Global lock
    bool initialized;
    XrTlsContext *tls_ctx;     // Shared TLS context
    uint64_t idle_timeout_ms;  // Idle timeout (default 60000ms)
} XrHttpConnPool;

/* ========== Connection Pool API ========== */

void http_conn_pool_destroy(XrHttpConnPool *pool);

// Get connection from pool, creates new one if none available. Requires
// the calling isolate so DNS resolution and netpoll suspension can be
// scheduled on the right runtime.
XrHttpPooledConn *http_conn_pool_get(struct XrVMRuntime *X, XrHttpConnPool *pool, const char *host,
                                     uint16_t port, bool is_https);

// Return connection to pool (closes if keep_alive=false).
void http_conn_pool_put(XrHttpConnPool *pool, XrHttpPooledConn *conn, const char *host,
                        uint16_t port, bool is_https, bool keep_alive);

void http_conn_pool_close(XrHttpPooledConn *conn);

/* Evict idle connections older than pool->idle_timeout_ms.
 * Designed to be called from a timer wheel callback. */
int http_conn_pool_evict_idle(XrHttpConnPool *pool);

/* ========== Per-Isolate Pool Creation ========== */

// Create a new HTTP connection pool (per-isolate)
XrHttpConnPool *http_conn_pool_new(void);

// Connection read/write helpers. The owning isolate is captured inside
// conn->io at creation time, so callers only pass the pooled connection.
int http_pooled_conn_read(XrHttpPooledConn *conn, void *buf, size_t len);
int http_pooled_conn_write(XrHttpPooledConn *conn, const void *buf, size_t len);

#endif  // XR_STDLIB_HTTP_CONN_POOL_H
