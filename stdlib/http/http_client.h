/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_client.h - HTTP client implementation
 *
 * KEY CONCEPT:
 *   Synchronous HTTP requests (GET/POST/PUT/DELETE), URL parsing,
 *   connection pooling, and timeout control.
 */

#ifndef XR_STDLIB_HTTP_CLIENT_H
#define XR_STDLIB_HTTP_CLIENT_H

#include "../../src/base/xdefs.h"
#include "http_parser.h"
#include "../net/xneterror.h"
#include <stdbool.h>

// Forward declaration
typedef struct XrayIsolate XrayIsolate;

/* ========== Constants ========== */

#define XR_HTTP_DEFAULT_PORT        80
#define XR_HTTP_DEFAULT_HTTPS_PORT  443
#define XR_HTTP_DEFAULT_TIMEOUT     30000   // 30 seconds
#define XR_HTTP_RECV_BUFFER_SIZE    8192
#define XR_HTTP_SEND_BUFFER_SIZE    4096

// HTTP error codes — aliases into unified XrNetError
typedef XrNetError XrHttpError;
#define XR_HTTP_OK              XR_NERR_OK
#define XR_HTTP_ERR_URL_PARSE   XR_NERR_URL_PARSE
#define XR_HTTP_ERR_DNS         XR_NERR_DNS
#define XR_HTTP_ERR_CONNECT     XR_NERR_CONNECT
#define XR_HTTP_ERR_SEND        XR_NERR_WRITE
#define XR_HTTP_ERR_RECV        XR_NERR_READ
#define XR_HTTP_ERR_TIMEOUT     XR_NERR_TIMEOUT
#define XR_HTTP_ERR_PARSE       XR_NERR_PARSE
#define XR_HTTP_ERR_TOO_LARGE   XR_NERR_TOO_LARGE
#define XR_HTTP_ERR_MEMORY      XR_NERR_MEMORY
#define XR_HTTP_ERR_TLS         XR_NERR_TLS

/* ========== URL Parse Result ========== */

typedef struct {
    char *scheme;       // http or https
    char *host;         // Hostname
    int port;           // Port number
    char *path;         // Path (with query string)
    bool is_https;      // Is HTTPS
} XrHttpUrl;

/* ========== HTTP Request Config ========== */

// Request context (for cancellation and timeout)
typedef struct XrHttpReqContext {
    volatile bool cancelled;        // Cancel flag
    int deadline_ms;                // Deadline (relative to request start)
    void *user_data;                // User data
} XrHttpReqContext;

typedef struct {
    const char *url;                // Request URL
    XrHttpMethod method;            // Request method
    const char *body;               // Request body
    size_t body_len;                // Request body length
    XrHttpHeader *headers;          // Custom headers
    int header_count;               // Header count
    int timeout_ms;                 // Timeout (milliseconds)
    bool follow_redirects;          // Follow redirects
    int max_redirects;              // Max redirect count
    XrHttpReqContext *ctx;          // Request context (optional)
    bool use_http2;                 // Force HTTP/2
    bool keep_alive;                // Use Keep-Alive
    bool stream;                    // Stream mode: return headers only, read body later
} XrHttpRequestConfig;

/* ========== HTTP Response Result ========== */

typedef struct {
    int status_code;                // Status code
    char *status_text;              // Status text (copied)
    XrHttpHeader *headers;          // Response headers (copied)
    int header_count;               // Header count
    char *body;                     // Response body (copied, needs free)
    size_t body_len;                // Response body length
    XrHttpError error;              // Error code
    char *error_msg;                // Error message
    // Stream mode fields (valid when stream=true in config)
    void *_stream_conn;             // XrPooledConn* kept open for body reads
    void *_stream_buf;              // XrNetBuffer* with leftover data after headers
    int64_t _stream_remaining;      // Remaining body bytes (-1 = chunked/unknown)
    bool _stream_chunked;           // Response uses chunked encoding
} XrHttpResult;

/* ========== API Functions ========== */

/*
 * Parse URL
 * Returns: 0 on success, -1 on failure
 * Note: caller must call xr_http_url_free to free
 */
XR_FUNC int xr_http_url_parse(const char *url, XrHttpUrl *out);

/*
 * Free URL structure
 */
XR_FUNC void xr_http_url_free(XrHttpUrl *url);

/*
 * Initialize request config
 */
XR_FUNC void xr_http_request_config_init(XrHttpRequestConfig *config);

/*
 * Execute HTTP request
 *
 * @param X       Isolate instance (for Cookie/proxy context)
 * @param config  Request config
 * @return        XrHttpResult (caller must call xr_http_result_free to free)
 */
XR_FUNC XrHttpResult xr_http_request(XrayIsolate *X, const XrHttpRequestConfig *config);

/*
 * Convenience function: GET request
 */
XR_FUNC XrHttpResult xr_http_get(XrayIsolate *X, const char *url);

/*
 * Convenience function: POST request
 */
XR_FUNC XrHttpResult xr_http_post(XrayIsolate *X, const char *url, const char *body, size_t body_len,
                          const char *content_type);

/*
 * Convenience function: PUT request
 */
XR_FUNC XrHttpResult xr_http_put(XrayIsolate *X, const char *url, const char *body, size_t body_len,
                         const char *content_type);

/*
 * Convenience function: DELETE request
 */
XR_FUNC XrHttpResult xr_http_delete(XrayIsolate *X, const char *url);

/*
 * Free response result
 */
XR_FUNC void xr_http_result_free(XrHttpResult *result);

/*
 * Read next chunk from a streaming response.
 * Returns bytes read (>0), 0 on EOF, -1 on error.
 * buf must be at least max_bytes in size.
 */
XR_FUNC int xr_http_stream_read(XrHttpResult *result, char *buf, int max_bytes);

/*
 * Close a streaming response and return connection to pool.
 * Must be called after stream reading is complete.
 */
XR_FUNC void xr_http_stream_close(XrHttpResult *result);

/*
 * Get error description
 */
XR_FUNC const char* xr_http_error_string(XrHttpError err);

/* ========== Request Context API ========== */

/*
 * Create request context
 */
XR_FUNC XrHttpReqContext* xr_http_context_new(void);

/*
 * Set timeout (milliseconds)
 */
XR_FUNC void xr_http_context_set_timeout(XrHttpReqContext *ctx, int timeout_ms);

/*
 * Cancel request
 */
XR_FUNC void xr_http_context_cancel(XrHttpReqContext *ctx);

/*
 * Check if cancelled
 */
XR_FUNC bool xr_http_context_is_cancelled(XrHttpReqContext *ctx);

/*
 * Free context
 */
XR_FUNC void xr_http_context_free(XrHttpReqContext *ctx);

// HTTP connection pool is managed per-Isolate via XrHttpContext.conn_pool
// (see http.h). There is no global pool — each Isolate owns its own pool,
// lazily created on the first HTTP request and destroyed when the Isolate
// tears down. Pool tuning (max_conns, idle_timeout) is done through the
// underlying XrConnPool API directly if needed.

#endif
