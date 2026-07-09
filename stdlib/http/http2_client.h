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
 *   ALPN negotiation, connection pooling, and multiplexed request streams.
 */

#ifndef XR_STDLIB_HTTP2_CLIENT_H
#define XR_STDLIB_HTTP2_CLIENT_H

#include "../../src/base/xdefs.h"
#include "http2.h"
#include "http_client.h"
#include "../net/tls.h"
#include <stdbool.h>
#include "../../src/os/os_thread.h"

/* ========== Connection Pool ========== */

#define XR_H2_POOL_MAX_CONNS 16        // Max connections per host
#define XR_H2_POOL_MAX_HOSTS 64        // Max number of hosts
#define XR_H2_CONN_IDLE_TIMEOUT 60000  // Idle timeout (milliseconds)

// Connection pool entry
typedef struct XrH2PoolEntry {
    char *host;                  // Hostname
    int port;                    // Port
    XrH2Conn *conn;              // HTTP/2 connection
    XrTlsConn *tls_conn;         // TLS connection
    XrTlsContext *tls_ctx;       // TLS context
    uint64_t last_used;          // Last used time
    int active_streams;          // Active stream count
    bool in_use;                 // Is in use
    struct XrH2PoolEntry *next;  // Next connection for same host
} XrH2PoolEntry;

// Connection pool
typedef struct XrH2Pool {
    XrH2PoolEntry *hosts[XR_H2_POOL_MAX_HOSTS];
    int host_count;
    xr_mutex_t lock;
    bool initialized;
} XrH2Pool;

/* ========== HTTP/2 Request ========== */

typedef struct XrH2Request {
    const char *method;     // Request method
    const char *path;       // Request path
    const char *authority;  // Host
    const char *scheme;     // http/https
    XrHttpHeader *headers;  // Additional headers
    int header_count;
    const char *body;  // Request body
    size_t body_len;
} XrH2Request;

// HTTP/2 response
typedef struct XrH2Response {
    int status;             // Status code
    XrHttpHeader *headers;  // Response headers
    int header_count;
    char *body;  // Response body
    size_t body_len;
    XrH2ErrorCode error;  // Error code
    char *error_msg;      // Error message
} XrH2Response;

/* ========== Internal HTTP/2 Client API ========== */

// Create per-isolate HTTP/2 connection pool
XrH2Pool *http2_client_pool_create(void);

// Free per-isolate pool
void http2_client_pool_destroy(XrH2Pool *pool);

// Acquire connection from per-isolate pool
XrH2PoolEntry *http2_client_pool_acquire(XrH2Pool *pool, const char *host, int port,
                                         bool is_https);

// Release connection to per-isolate pool
void http2_client_pool_release(XrH2Pool *pool, XrH2PoolEntry *entry);

/*
 * Send HTTP/2 request
 *
 * pool: Per-isolate HTTP/2 connection pool
 * url: Request URL
 * req: Request parameters
 *
 * Returns: response (must call http2_client_response_free to free)
 */
XrH2Response *http2_client_request(XrH2Pool *pool, const char *url, const XrH2Request *req);

/*
 * Free response
 */
void http2_client_response_free(XrH2Response *resp);

#endif
