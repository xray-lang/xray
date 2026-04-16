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

#include <stdbool.h>
#include <stddef.h>

// Forward declaration
typedef struct XrayIsolate XrayIsolate;

/* ========== Proxy Config ========== */

typedef struct XrProxyConfig {
    char *host;             // Proxy host
    int port;               // Proxy port
    char *username;         // Username (optional)
    char *password;         // Password (optional)
    bool use_connect;       // Use CONNECT method (for HTTPS)
} XrProxyConfig;

/* ========== API ========== */

/*
 * Parse proxy URL
 * Format: http://[user:pass@]host:port
 * 
 * Returns: 0 on success, -1 on failure
 */
int xr_proxy_parse(const char *proxy_url, XrProxyConfig *out);

/*
 * Free proxy config
 */
void xr_proxy_config_free(XrProxyConfig *config);

/*
 * Build proxy auth header (Base64 encoded)
 * Returns: newly allocated string (caller must free)
 */
char* xr_proxy_auth_header(const char *username, const char *password);

/*
 * Build CONNECT request (for HTTPS proxy)
 * Returns: newly allocated string (caller must free)
 */
char* xr_proxy_connect_request(const char *target_host, int target_port,
                                const char *proxy_auth);

/*
 * Parse CONNECT response
 * Returns: status code (200 = success)
 */
int xr_proxy_parse_connect_response(const char *response, size_t len);

/* ========== Proxy Settings (Per-Isolate) ========== */

/*
 * Set proxy
 */
void xr_set_proxy(XrayIsolate *X, const char *proxy_url);

/*
 * Get proxy config
 */
XrProxyConfig* xr_get_proxy(XrayIsolate *X);

/*
 * Clear proxy
 */
void xr_clear_proxy(XrayIsolate *X);

/*
 * Check if proxy should be used
 */
bool xr_should_use_proxy(XrayIsolate *X, const char *host);

/*
 * Add to no_proxy list
 */
void xr_add_no_proxy(XrayIsolate *X, const char *host);

#endif
