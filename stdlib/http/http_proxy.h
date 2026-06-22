/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_proxy.h - HTTP proxy support
 *
 * KEY CONCEPT:
 *   HTTP proxy, HTTPS proxy (CONNECT tunnel), and Basic authentication.
 */

#ifndef XR_STDLIB_HTTP_PROXY_H
#define XR_STDLIB_HTTP_PROXY_H

#include "../../src/base/xdefs.h"
#include <stdbool.h>
#include <stddef.h>

// Forward declaration
typedef struct XrVMRuntime XrVMRuntime;

/* ========== Proxy Config ========== */

typedef struct XrProxyConfig {
    char *host;        // Proxy host
    int port;          // Proxy port
    char *username;    // Username (optional)
    char *password;    // Password (optional)
    bool use_connect;  // Use CONNECT method (for HTTPS)
} XrProxyConfig;

/* ========== API ========== */

/*
 * Parse proxy URL
 * Format: http://[user:pass@]host:port
 *
 * Returns: 0 on success, -1 on failure
 */
XR_FUNC int xr_proxy_parse(const char *proxy_url, XrProxyConfig *out);

/*
 * Free proxy config
 */
XR_FUNC void xr_proxy_config_free(XrProxyConfig *config);

/*
 * Build proxy auth header (Base64 encoded)
 * Returns: newly allocated string (caller must free)
 */
XR_FUNC char *xr_proxy_auth_header(const char *username, const char *password);

/*
 * Build CONNECT request (for HTTPS proxy)
 * Returns: newly allocated string (caller must free)
 */
XR_FUNC char *xr_proxy_connect_request(const char *target_host, int target_port,
                                       const char *proxy_auth);

/*
 * Parse CONNECT response
 * Returns: status code (200 = success)
 */
XR_FUNC int xr_proxy_parse_connect_response(const char *response, size_t len);

/* ========== Proxy Settings (Per-Isolate) ========== */

/*
 * Set proxy
 */
XR_FUNC void xr_set_proxy(XrVMRuntime *X, const char *proxy_url);

/*
 * Get proxy config
 */
XR_FUNC XrProxyConfig *xr_get_proxy(XrVMRuntime *X);

/*
 * Clear proxy
 */
XR_FUNC void xr_clear_proxy(XrVMRuntime *X);

/*
 * Check if proxy should be used
 */
XR_FUNC bool xr_should_use_proxy(XrVMRuntime *X, const char *host);

/*
 * Add to no_proxy list
 */
XR_FUNC void xr_add_no_proxy(XrVMRuntime *X, const char *host);

#endif
