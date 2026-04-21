/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_proxy.c - HTTP proxy support implementation
 *
 * KEY CONCEPT:
 *   HTTP/HTTPS proxy with CONNECT tunnel and authentication
 */

#include "../../src/base/xmalloc.h"
#include "http_proxy.h"
#include "http.h"
#include "../base64/base64.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ========== Proxy Configuration Parsing ========== */

int xr_proxy_parse(const char *proxy_url, XrProxyConfig *out) {
    if (!proxy_url || !out) return -1;
    
    memset(out, 0, sizeof(XrProxyConfig));
    
    const char *p = proxy_url;
    
    // Skip scheme
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        out->use_connect = true;
    }
    
    // Check for authentication info
    const char *at = strchr(p, '@');
    if (at) {
        // Extract username and password
        const char *colon = strchr(p, ':');
        if (colon && colon < at) {
            out->username = strndup(p, colon - p);
            out->password = strndup(colon + 1, at - colon - 1);
        } else {
            out->username = strndup(p, at - p);
        }
        p = at + 1;
    }
    
    // Extract host and port
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    
    if (colon && (!slash || colon < slash)) {
        out->host = strndup(p, colon - p);
        out->port = atoi(colon + 1);
    } else if (slash) {
        out->host = strndup(p, slash - p);
        out->port = 8080;  // Default proxy port
    } else {
        out->host = xr_strdup(p);
        out->port = 8080;
    }
    
    if (!out->host || out->port <= 0) {
        xr_proxy_config_free(out);
        return -1;
    }
    
    return 0;
}

void xr_proxy_config_free(XrProxyConfig *config) {
    if (!config) return;
    xr_free(config->host);
    xr_free(config->username);
    xr_free(config->password);
    memset(config, 0, sizeof(XrProxyConfig));
}

/* ========== Proxy Authentication ========== */

char* xr_proxy_auth_header(const char *username, const char *password) {
    if (!username) return NULL;
    
    // Build user:pass string
    size_t user_len = strlen(username);
    size_t pass_len = password ? strlen(password) : 0;
    size_t cred_len = user_len + 1 + pass_len;
    
    char *credentials = (char*)xr_malloc(cred_len + 1);
    if (!credentials) return NULL;
    
    if (password) {
        snprintf(credentials, cred_len + 1, "%s:%s", username, password);
    } else {
        snprintf(credentials, cred_len + 1, "%s:", username);
    }
    
    // Base64 encode
    char *encoded = xr_base64_encode((const unsigned char*)credentials, strlen(credentials), NULL);
    xr_free(credentials);
    
    if (!encoded) return NULL;
    
    // Build complete header
    size_t header_len = 7 + strlen(encoded) + 1;  // "Basic " + encoded + \0
    char *header = (char*)xr_malloc(header_len);
    if (!header) {
        xr_free(encoded);
        return NULL;
    }
    
    snprintf(header, header_len, "Basic %s", encoded);
    xr_free(encoded);
    
    return header;
}

/* ========== CONNECT Request ========== */

char* xr_proxy_connect_request(const char *target_host, int target_port,
                                const char *proxy_auth) {
    if (!target_host) return NULL;
    
    size_t buf_size = 512;
    if (proxy_auth) buf_size += strlen(proxy_auth);
    
    char *buf = (char*)xr_malloc(buf_size);
    if (!buf) return NULL;
    
    char *p = buf;
    p += sprintf(p, "CONNECT %s:%d HTTP/1.1\r\n", target_host, target_port);
    p += sprintf(p, "Host: %s:%d\r\n", target_host, target_port);
    
    if (proxy_auth) {
        p += sprintf(p, "Proxy-Authorization: %s\r\n", proxy_auth);
    }
    
    p += sprintf(p, "User-Agent: xray-http/1.0\r\n");
    p += sprintf(p, "Proxy-Connection: Keep-Alive\r\n");
    p += sprintf(p, "\r\n");
    
    return buf;
}

int xr_proxy_parse_connect_response(const char *response, size_t len) {
    if (!response || len < 12) return -1;
    
    // Check HTTP/1.x response
    if (strncmp(response, "HTTP/1.", 7) != 0) return -1;
    
    // Extract status code
    const char *status_start = response + 9;
    return atoi(status_start);
}

/* ========== Proxy Configuration (managed via XrHttpContext) ========== */

void xr_set_proxy(XrayIsolate *X, const char *proxy_url) {
    xr_clear_proxy(X);
    
    if (!proxy_url) return;
    
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) return;
    
    ctx->proxy = (XrProxyConfig*)xr_calloc(1, sizeof(XrProxyConfig));
    if (ctx->proxy) {
        if (xr_proxy_parse(proxy_url, ctx->proxy) < 0) {
            xr_free(ctx->proxy);
            ctx->proxy = NULL;
        }
    }
}

XrProxyConfig* xr_get_proxy(XrayIsolate *X) {
    XrHttpContext *ctx = xr_http_get_context(X);
    return ctx ? ctx->proxy : NULL;
}

void xr_clear_proxy(XrayIsolate *X) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) return;
    
    if (ctx->proxy) {
        xr_proxy_config_free(ctx->proxy);
        xr_free(ctx->proxy);
        ctx->proxy = NULL;
    }
    
    // Clear no_proxy list
    for (int i = 0; i < ctx->no_proxy_count; i++) {
        xr_free(ctx->no_proxy[i]);
    }
    xr_free(ctx->no_proxy);
    ctx->no_proxy = NULL;
    ctx->no_proxy_count = 0;
}

bool xr_should_use_proxy(XrayIsolate *X, const char *host) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx || !ctx->proxy || !host) return false;
    
    // Check no_proxy list
    for (int i = 0; i < ctx->no_proxy_count; i++) {
        if (strcasecmp(ctx->no_proxy[i], host) == 0) {
            return false;
        }
        // Support wildcard suffix matching (e.g. .example.com)
        if (ctx->no_proxy[i][0] == '.') {
            size_t suffix_len = strlen(ctx->no_proxy[i]);
            size_t host_len = strlen(host);
            if (host_len >= suffix_len) {
                if (strcasecmp(host + host_len - suffix_len, ctx->no_proxy[i]) == 0) {
                    return false;
                }
            }
        }
    }
    
    // localhost defaults to no proxy
    if (strcasecmp(host, "localhost") == 0 || 
        strcmp(host, "127.0.0.1") == 0 ||
        strcmp(host, "::1") == 0) {
        return false;
    }
    
    return true;
}

void xr_add_no_proxy(XrayIsolate *X, const char *host) {
    if (!host) return;
    
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) return;
    
    ctx->no_proxy = (char**)xr_realloc(ctx->no_proxy, sizeof(char*) * (ctx->no_proxy_count + 1));
    if (ctx->no_proxy) {
        ctx->no_proxy[ctx->no_proxy_count++] = xr_strdup(host);
    }
}
