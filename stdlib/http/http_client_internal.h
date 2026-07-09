/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_client_internal.h - Internal HTTP/1.x client native boundary
 *
 * KEY CONCEPT:
 *   Internal HTTP/1.x client data-plane used by http.request and xpkg.
 *   User-facing protocol helpers live in stdlib/http/http.xr.
 */

#ifndef XR_STDLIB_HTTP_CLIENT_INTERNAL_H
#define XR_STDLIB_HTTP_CLIENT_INTERNAL_H

#include "../../src/base/xdefs.h"
#include "http_parser_internal.h"
#include "../net/xneterror.h"
#include <stdbool.h>

// Forward declaration
typedef struct XrVMRuntime XrVMRuntime;

/* ========== Constants ========== */

#define XR_HTTP_DEFAULT_PORT 80
#define XR_HTTP_DEFAULT_HTTPS_PORT 443
#define XR_HTTP_DEFAULT_TIMEOUT 30000  // 30 seconds
#define XR_HTTP_RECV_BUFFER_SIZE 8192
#define XR_HTTP_SEND_BUFFER_SIZE 4096

// HTTP error codes — aliases into unified XrNetError
typedef XrNetError XrHttpError;
#define XR_HTTP_OK XR_NERR_OK
#define XR_HTTP_ERR_URL_PARSE XR_NERR_URL_PARSE
#define XR_HTTP_ERR_DNS XR_NERR_DNS
#define XR_HTTP_ERR_CONNECT XR_NERR_CONNECT
#define XR_HTTP_ERR_SEND XR_NERR_WRITE
#define XR_HTTP_ERR_RECV XR_NERR_READ
#define XR_HTTP_ERR_TIMEOUT XR_NERR_TIMEOUT
#define XR_HTTP_ERR_PARSE XR_NERR_PARSE
#define XR_HTTP_ERR_TOO_LARGE XR_NERR_TOO_LARGE
#define XR_HTTP_ERR_MEMORY XR_NERR_MEMORY
#define XR_HTTP_ERR_TLS XR_NERR_TLS

/* ========== URL Parse Result ========== */

typedef struct {
    char *scheme;   // http or https
    char *host;     // Hostname
    int port;       // Port number
    char *path;     // Path (with query string)
    bool is_https;  // Is HTTPS
} XrHttpUrl;

/* ========== HTTP Request Config ========== */

typedef struct {
    const char *url;          // Request URL
    XrHttpMethod method;      // Request method
    const char *method_name;  // Optional normalized method token
    size_t method_name_len;   // Length of method_name
    const char *body;         // Request body
    size_t body_len;          // Request body length
    XrHttpHeader *headers;    // Custom headers
    int header_count;         // Header count
    int timeout_ms;           // Timeout (milliseconds)
    bool follow_redirects;    // Follow redirects
    int max_redirects;        // Max redirect count
    bool use_http2;           // Force HTTP/2
    bool keep_alive;          // Use Keep-Alive
} XrHttpRequestConfig;

/* ========== HTTP Response Result ========== */

typedef struct {
    int status_code;        // Status code
    char *status_text;      // Status text (copied)
    XrHttpHeader *headers;  // Response headers (zero-copy into _header_data)
    int header_count;       // Header count
    char *_header_data;     // Single allocation backing all header name/value strings
    char *body;             // Response body (copied, needs free)
    size_t body_len;        // Response body length
    XrHttpError error;      // Error code
    char *error_msg;        // Error message
} XrHttpResult;

/* ========== Internal Client API ========== */

/*
 * Parse URL
 * Returns: 0 on success, -1 on failure
 * Note: caller must call http_url_free to free
 */
int http_url_parse(const char *url, XrHttpUrl *out);

/*
 * Free URL structure
 */
void http_url_free(XrHttpUrl *url);

/*
 * Initialize request config
 */
void http_client_request_config_init(XrHttpRequestConfig *config);

/*
 * Execute HTTP request
 *
 * @param X       Isolate instance (for per-isolate connection pools)
 * @param config  Request config
 * @return        XrHttpResult (caller must call http_client_result_free to free)
 */
XrHttpResult http_client_request(XrVMRuntime *X, const XrHttpRequestConfig *config);

/*
 * Free response result
 */
void http_client_result_free(XrHttpResult *result);

/*
 * Get error description
 */
const char *http_client_error_string(XrHttpError err);

// HTTP connection pool is managed per-Isolate via XrHttpContext.conn_pool
// (see http.h). There is no global pool — each Isolate owns its own pool,
// lazily created on the first HTTP request and destroyed when the Isolate
// tears down. Pool tuning (max_conns, idle_timeout) is done through the
// underlying XrConnPool API directly if needed.

#endif
