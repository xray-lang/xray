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

#include "http2.h"
#include "http_client.h"

/* ========== Connection Pool ========== */

typedef struct XrH2Pool XrH2Pool;

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
