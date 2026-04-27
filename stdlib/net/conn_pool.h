/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * conn_pool.h - HTTP connection pool
 *
 * KEY CONCEPT:
 *   Manages persistent TCP/TLS connections grouped by host:port:https.
 *   Supports Keep-Alive, idle timeout cleanup, and thread-safe access.
 */

#ifndef XR_STDLIB_CONN_POOL_H
#define XR_STDLIB_CONN_POOL_H

#include "../../src/base/xdefs.h"
#include "tls.h"
#include <stdint.h>
#include <stdbool.h>
#include "../../src/os/os_thread.h"

/* ========== Constants ========== */

#define XR_POOL_MAX_CONNS_PER_HOST 6  // Max connections per host
#define XR_POOL_MAX_IDLE_TIME 60      // Idle timeout in seconds
#define XR_POOL_MAX_HOSTS 64          // Max number of hosts
#define XR_POOL_HOST_KEY_LEN 256      // Max length of host key

/* ========== Connection State ========== */

typedef enum {
    XR_CONN_IDLE = 0,
    XR_CONN_IN_USE,
    XR_CONN_CLOSED
} XrConnState;

/* ========== Pooled Connection ========== */

typedef struct XrPooledConn {
    int fd;
    XrTlsConn *tls_conn;  // TLS connection (NULL for plain TCP)
    XrConnState state;
    uint64_t last_used_ms;  // Monotonic timestamp (milliseconds)
    uint64_t created_ms;
    bool is_https;
    struct XrPooledConn *next;
} XrPooledConn;

/* ========== Host Pool ========== */

typedef struct XrHostPool {
    char key[XR_POOL_HOST_KEY_LEN];  // Format: "host:port:https"
    char host[128];
    uint16_t port;
    bool is_https;
    XrPooledConn *conns;  // Connection list
    int conn_count;
    int idle_count;
    struct XrHostPool *next;  // Hash collision chain
} XrHostPool;

/* ========== Global Connection Pool ========== */

typedef struct XrConnPool {
    XrHostPool *buckets[XR_POOL_MAX_HOSTS];  // Hash buckets
    xr_mutex_t lock;                         // Global lock
    int total_conns;
    bool initialized;
    XrTlsContext *tls_ctx;     // Shared TLS context
    uint64_t idle_timeout_ms;  // Idle timeout (default 60000ms)
} XrConnPool;

/* ========== Connection Pool API ========== */

XR_FUNC void xr_conn_pool_init(XrConnPool *pool);
XR_FUNC void xr_conn_pool_destroy(XrConnPool *pool);

// Get connection from pool, creates new one if none available
XR_FUNC XrPooledConn *xr_conn_pool_get(XrConnPool *pool, const char *host, uint16_t port,
                                       bool is_https);

// Return connection to pool (closes if keep_alive=false)
XR_FUNC void xr_conn_pool_put(XrConnPool *pool, XrPooledConn *conn, const char *host, uint16_t port,
                              bool is_https, bool keep_alive);

XR_FUNC void xr_conn_pool_close(XrConnPool *pool, XrPooledConn *conn);

/* Evict idle connections older than pool->idle_timeout_ms.
 * Designed to be called from a timer wheel callback. */
XR_FUNC int xr_conn_pool_evict_idle(XrConnPool *pool);

XR_FUNC void xr_conn_pool_cleanup(XrConnPool *pool);
XR_FUNC void xr_conn_pool_stats(XrConnPool *pool, int *total, int *idle);

/* ========== Per-Isolate Pool Creation ========== */

// Create a new connection pool (per-isolate)
XR_FUNC XrConnPool *xr_conn_pool_new(void);

// Connection read/write helpers
XR_FUNC int xr_pooled_conn_read(XrPooledConn *conn, void *buf, size_t len);
XR_FUNC int xr_pooled_conn_write(XrPooledConn *conn, const void *buf, size_t len);

#endif  // XR_STDLIB_CONN_POOL_H
