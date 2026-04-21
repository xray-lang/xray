/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_client.h - HTTP/2 client implementation
 *
 * KEY CONCEPT:
 *   ALPN auto-negotiation, connection pooling, multiplexed streams,
 *   and stream priority support.
 */

#ifndef XR_STDLIB_HTTP2_CLIENT_H
#define XR_STDLIB_HTTP2_CLIENT_H

#include "../../src/base/xdefs.h"
#include "http2.h"
#include "http_client.h"
#include "../net/tls.h"
#include <stdbool.h>
#include <pthread.h>

/* ========== Connection Pool ========== */

#define XR_H2_POOL_MAX_CONNS    16      // Max connections per host
#define XR_H2_POOL_MAX_HOSTS    64      // Max number of hosts
#define XR_H2_CONN_IDLE_TIMEOUT 60000   // Idle timeout (milliseconds)

// Connection pool entry
typedef struct XrH2PoolEntry {
    char *host;                     // Hostname
    int port;                       // Port
    XrH2Conn *conn;                 // HTTP/2 connection
    XrTlsConn *tls_conn;            // TLS connection
    XrTlsContext *tls_ctx;          // TLS context
    uint64_t last_used;             // Last used time
    int active_streams;             // Active stream count
    bool in_use;                    // Is in use
    struct XrH2PoolEntry *next;     // Next connection for same host
} XrH2PoolEntry;

// Connection pool
typedef struct XrH2Pool {
    XrH2PoolEntry *hosts[XR_H2_POOL_MAX_HOSTS];
    int host_count;
    pthread_mutex_t lock;
    bool initialized;
} XrH2Pool;

/* ========== HTTP/2 Request ========== */

typedef struct XrH2Request {
    const char *method;             // Request method
    const char *path;               // Request path
    const char *authority;          // Host
    const char *scheme;             // http/https
    XrHttpHeader *headers;          // Additional headers
    int header_count;
    const char *body;               // Request body
    size_t body_len;
} XrH2Request;

// HTTP/2 response
typedef struct XrH2Response {
    int status;                     // Status code
    XrHttpHeader *headers;          // Response headers
    int header_count;
    char *body;                     // Response body
    size_t body_len;
    XrH2ErrorCode error;            // Error code
    char *error_msg;                // Error message
} XrH2Response;

/* ========== Per-Isolate Pool API ========== */

// Create per-isolate HTTP/2 connection pool
XR_FUNC XrH2Pool* xr_h2_pool_create(void);

// Free per-isolate pool
XR_FUNC void xr_h2_pool_destroy(XrH2Pool *pool);

// Acquire connection from per-isolate pool
XR_FUNC XrH2PoolEntry* xr_h2_pool_acquire_from(XrH2Pool *pool, const char *host, int port, bool is_https);

// Release connection to per-isolate pool
XR_FUNC void xr_h2_pool_release_to(XrH2Pool *pool, XrH2PoolEntry *entry);

/* ========== Legacy Global API (deprecated) ========== */

// Initialize global connection pool
XR_FUNC void xr_h2_pool_init(void);

// Cleanup global connection pool
XR_FUNC void xr_h2_pool_cleanup(void);

// Acquire from global pool (deprecated)
XR_FUNC XrH2PoolEntry* xr_h2_pool_acquire(const char *host, int port, bool is_https);

// Release to global pool (deprecated)
XR_FUNC void xr_h2_pool_release(XrH2PoolEntry *entry);

/*
 * Send HTTP/2 request
 * 
 * url: Request URL
 * req: Request parameters
 * 
 * Returns: response (must call xr_h2_response_free to free)
 */
XR_FUNC XrH2Response* xr_h2_request(const char *url, const XrH2Request *req);

/*
 * Convenience function: HTTP/2 GET
 */
XR_FUNC XrH2Response* xr_h2_get(const char *url);

/*
 * Convenience function: HTTP/2 POST
 */
XR_FUNC XrH2Response* xr_h2_post(const char *url, const char *body, size_t body_len,
                          const char *content_type);

/*
 * Free response
 */
XR_FUNC void xr_h2_response_free(XrH2Response *resp);

/*
 * Auto-select HTTP version
 * 
 * Automatically selects HTTP/1.1 or HTTP/2 based on ALPN negotiation result
 * 
 * Returns: true = HTTP/2, false = HTTP/1.1
 */
XR_FUNC bool xr_http_auto_version(const char *host, int port, bool is_https);

/*
 * Cleanup idle connections
 */
XR_FUNC void xr_h2_pool_cleanup_idle(void);

#endif
