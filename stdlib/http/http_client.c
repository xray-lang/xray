/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_client.c - HTTP client implementation
 *
 * KEY CONCEPT:
 *   - Coroutine-friendly HTTP client backed by conn_pool (non-blocking I/O)
 *   - URL parsing (RFC 3986, IPv6 literal support)
 *   - Timeout control
 */

#include "../../src/base/xmalloc.h"
#include "http_client.h"
#include "http_parser.h"
#include "../../src/base/xplatform.h"
#include "http.h"
#include "../net/tls.h"
#include "http_cookie.h"
#include "../compress/compress.h"
#include "../../src/io/xdns.h"
#include "../net/io.h"
#include "../net/xnetbuf.h"
#include "../net/conn_pool.h"
#include "../../src/os/os_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Per-Isolate Connection Pool ========== */

// Return the connection pool owned by this Isolate's XrHttpContext, lazily
// constructing it on first use. Returns NULL if the context cannot be
// resolved or pool allocation fails.
//
// Historically a process-global `g_http_pool` was used, which leaked across
// Isolates and caused cross-Isolate fd/TLS reuse. We now store the pool on
// XrHttpContext so each Isolate has its own, and it is freed by
// xr_http_module_context_free() during Isolate teardown.
static XrConnPool *http_client_pool(XrayIsolate *X) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx)
        return NULL;
    if (!ctx->conn_pool) {
        ctx->conn_pool = xr_conn_pool_new();
    }
    return ctx->conn_pool;
}

/* ========== URL Parsing ========== */

int xr_http_url_parse(const char *url, XrHttpUrl *out) {
    if (!url || !out)
        return -1;

    memset(out, 0, sizeof(XrHttpUrl));

    const char *p = url;

    // Parse scheme
    const char *scheme_end = strstr(p, "://");
    if (!scheme_end) {
        // Default http
        out->scheme = xr_strdup("http");
        out->is_https = false;
        out->port = XR_HTTP_DEFAULT_PORT;
    } else {
        int scheme_len = (int) (scheme_end - p);
        out->scheme = (char *) xr_malloc(scheme_len + 1);
        if (!out->scheme)
            return -1;
        memcpy(out->scheme, p, scheme_len);
        out->scheme[scheme_len] = '\0';

        // Convert to lowercase
        for (int i = 0; i < scheme_len; i++) {
            if (out->scheme[i] >= 'A' && out->scheme[i] <= 'Z') {
                out->scheme[i] += 'a' - 'A';
            }
        }

        out->is_https = (strcmp(out->scheme, "https") == 0);
        out->port = out->is_https ? XR_HTTP_DEFAULT_HTTPS_PORT : XR_HTTP_DEFAULT_PORT;

        p = scheme_end + 3;
    }

    // Skip optional userinfo (`user:pass@`). We do not surface credentials
    // to callers — they should use config->headers for Authorization. We
    // only scan until the authority delimiter (/ ? #) to avoid matching an
    // '@' inside the path/query. This is RFC 3986 compliant stripping, not
    // validation; malformed userinfo is silently dropped.
    {
        const char *scan = p;
        const char *at = NULL;
        while (*scan && *scan != '/' && *scan != '?' && *scan != '#') {
            if (*scan == '@') {
                at = scan;
                break;
            }
            if (*scan == '[') {
                // IPv6 host begins; any earlier '@' would have been found
                // above. Since we didn't, there is no userinfo — stop.
                break;
            }
            scan++;
        }
        if (at)
            p = at + 1;
    }

    // Parse host + port. Two shapes per RFC 3986 §3.2.2:
    //   1. reg-name / IPv4:  host[:port]
    //   2. IP-literal:       [IPv6-addr][:port]   — brackets are mandatory
    //                                               so ':' inside v6 is
    //                                               unambiguous.
    const char *host_start;
    const char *host_end;
    const char *port_start = NULL;

    if (*p == '[') {
        // IPv6 literal. Host is the content between the brackets. We do
        // not validate the v6 syntax here — DNS resolver / inet_pton will
        // reject malformed addresses later with a clean error.
        host_start = p + 1;
        const char *close = strchr(p, ']');
        if (!close) {
            xr_http_url_free(out);
            return -1;
        }
        host_end = close;
        p = close + 1;
        if (*p == ':') {
            port_start = p + 1;
            p++;
            while (*p >= '0' && *p <= '9')
                p++;
        }
        // Consume remaining authority chars up to path/query/fragment.
        while (*p && *p != '/' && *p != '?' && *p != '#')
            p++;
    } else {
        // reg-name or IPv4. Scan to first ':' (port delimiter) or authority
        // terminator. Since reg-name / IPv4 contain no colons, the first
        // ':' unambiguously starts the port.
        host_start = p;
        host_end = NULL;
        while (*p && *p != '/' && *p != '?' && *p != '#') {
            if (*p == ':' && !port_start) {
                host_end = p;
                port_start = p + 1;
            }
            p++;
        }
        if (!host_end)
            host_end = p;
    }

    // Copy host
    int host_len = (int) (host_end - host_start);
    if (host_len == 0) {
        xr_http_url_free(out);
        return -1;
    }

    out->host = (char *) xr_malloc(host_len + 1);
    if (!out->host)
        return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    // Parse port
    if (port_start) {
        const char *port_end = p;
        // Walk forward to the first non-digit if port_start came from the
        // IPv6 branch (which didn't set `p` to the port boundary); this is
        // a no-op for the reg-name branch where `p == port_end` already.
        int port = 0;
        while (port_start < port_end && *port_start >= '0' && *port_start <= '9') {
            port = port * 10 + (*port_start - '0');
            port_start++;
        }
        if (port_start != port_end) {
            // Non-digit before authority terminator → invalid port
            xr_http_url_free(out);
            return -1;
        }
        if (port > 0 && port <= 65535) {
            out->port = port;
        }
    }

    // Parse path (including query string)
    if (*p) {
        out->path = xr_strdup(p);
    } else {
        out->path = xr_strdup("/");
    }

    return 0;
}

void xr_http_url_free(XrHttpUrl *url) {
    if (!url)
        return;
    if (url->scheme) {
        xr_free(url->scheme);
        url->scheme = NULL;
    }
    if (url->host) {
        xr_free(url->host);
        url->host = NULL;
    }
    if (url->path) {
        xr_free(url->path);
        url->path = NULL;
    }
}

/* ========== Request Config Initialization ========== */

void xr_http_request_config_init(XrHttpRequestConfig *config) {
    memset(config, 0, sizeof(XrHttpRequestConfig));
    config->method = XR_HTTP_METHOD_GET;
    config->timeout_ms = XR_HTTP_DEFAULT_TIMEOUT;
    config->follow_redirects = true;
    config->max_redirects = 5;
}

/* ========== Error Description ========== */

const char *xr_http_error_string(XrHttpError err) {
    return xr_net_error_string(err);
}

/* ========== Compact Header Copy (single allocation) ========== */

/*
 * Copy parsed headers into result using two allocations total:
 *   1. XrHttpHeader array
 *   2. One contiguous char buffer for all name + NUL + value + NUL strings
 * Header name/value pointers in the array point into (2).
 * Returns 0 on success, -1 on allocation failure.
 */
static int copy_headers_compact(XrHttpResult *result, const XrHttpHeader *src, size_t count) {
    if (count == 0)
        return 0;

    // Pass 1: compute total string bytes
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += src[i].name_len + 1 + src[i].value_len + 1;
    }

    XrHttpHeader *hdrs = (XrHttpHeader *) xr_malloc(sizeof(XrHttpHeader) * count);
    if (!hdrs)
        return -1;
    char *data = (char *) xr_malloc(total);
    if (!data) {
        xr_free(hdrs);
        return -1;
    }

    // Pass 2: pack strings
    char *p = data;
    for (size_t i = 0; i < count; i++) {
        hdrs[i].name = p;
        hdrs[i].name_len = src[i].name_len;
        memcpy(p, src[i].name, src[i].name_len);
        p += src[i].name_len;
        *p++ = '\0';

        hdrs[i].value = p;
        hdrs[i].value_len = src[i].value_len;
        memcpy(p, src[i].value, src[i].value_len);
        p += src[i].value_len;
        *p++ = '\0';
    }

    result->headers = hdrs;
    result->header_count = (int) count;
    result->_header_data = data;
    return 0;
}

/* ========== Result Cleanup ========== */

void xr_http_result_free(XrHttpResult *result) {
    if (!result)
        return;
    if (result->status_text) {
        xr_free(result->status_text);
        result->status_text = NULL;
    }
    if (result->_header_data) {
        xr_free(result->_header_data);
        result->_header_data = NULL;
    }
    if (result->headers) {
        xr_free(result->headers);
        result->headers = NULL;
    }
    if (result->body) {
        xr_free(result->body);
        result->body = NULL;
    }
    if (result->error_msg) {
        xr_free(result->error_msg);
        result->error_msg = NULL;
    }
    // Clean up stream resources if caller forgot to close. Result-free
    // is called during isolate teardown / GC finalize where the calling
    // isolate is not in scope; pass NULL so the inner pool close skips
    // netpoll deregistration (the fd is going away anyway).
    if (result->_stream_conn || result->_stream_buf) {
        xr_http_stream_close(NULL, result);
    }
}

/* ========== Build Request ========== */

static char *build_request(XrayIsolate *isolate, const XrHttpRequestConfig *config,
                           const XrHttpUrl *url, size_t *out_len) {
    // Estimate buffer size
    size_t buf_size = 1024;
    if (config->body_len > 0) {
        buf_size += config->body_len;
    }
    buf_size += strlen(url->path) + strlen(url->host);
    for (int i = 0; i < config->header_count; i++) {
        buf_size += config->headers[i].name_len + config->headers[i].value_len + 4;
    }

    char *buf = (char *) xr_malloc(buf_size);
    if (!buf)
        return NULL;

    char *p = buf;

    // Request line
    const char *method = xr_http_method_to_string(config->method);
    p += sprintf(p, "%s %s HTTP/1.1\r\n", method, url->path);

    // Custom Headers
    bool has_content_length = false;
    bool has_user_agent = false;
    bool has_accept = false;
    bool has_connection = false;
    bool has_host = false;

    // First pass: check if Host header exists in custom headers
    for (int i = 0; i < config->header_count; i++) {
        XrHttpHeader *h = &config->headers[i];
        if (h->name_len == 4 && strncasecmp(h->name, "Host", 4) == 0) {
            has_host = true;
            break;
        }
    }

    for (int i = 0; i < config->header_count; i++) {
        XrHttpHeader *h = &config->headers[i];
        memcpy(p, h->name, h->name_len);
        p += h->name_len;
        *p++ = ':';
        *p++ = ' ';
        memcpy(p, h->value, h->value_len);
        p += h->value_len;
        *p++ = '\r';
        *p++ = '\n';

        // Check special headers (switch on length for jump table)
        switch (h->name_len) {
            case 6:
                if (strncasecmp(h->name, "Accept", 6) == 0)
                    has_accept = true;
                break;
            case 10:
                if (strncasecmp(h->name, "User-Agent", 10) == 0)
                    has_user_agent = true;
                else if (strncasecmp(h->name, "Connection", 10) == 0)
                    has_connection = true;
                break;
            case 14:
                if (strncasecmp(h->name, "Content-Length", 14) == 0)
                    has_content_length = true;
                break;
            default:
                break;
        }
    }

    // Add auto Host header only if not provided by custom headers.
    // IPv6 literals MUST be wrapped in '[...]' per RFC 7230 §5.4 so the
    // colons in the address are unambiguous against the port separator.
    // We detect v6 by scanning the hostname for ':' — reg-names and IPv4
    // addresses never contain one.
    if (!has_host) {
        bool is_v6 = (strchr(url->host, ':') != NULL);
        bool default_port =
            (url->port == XR_HTTP_DEFAULT_PORT || url->port == XR_HTTP_DEFAULT_HTTPS_PORT);
        if (default_port) {
            p += sprintf(p, is_v6 ? "Host: [%s]\r\n" : "Host: %s\r\n", url->host);
        } else {
            p += sprintf(p, is_v6 ? "Host: [%s]:%d\r\n" : "Host: %s:%d\r\n", url->host, url->port);
        }
    }

    // Default Headers
    if (!has_user_agent) {
        p += sprintf(p, "User-Agent: xray-http/1.0\r\n");
    }
    if (!has_accept) {
        p += sprintf(p, "Accept: */*\r\n");
    }
    if (!has_connection) {
        p += sprintf(p, "Connection: keep-alive\r\n");
    }

    // Accept-Encoding (supports gzip decompression)
    { p += sprintf(p, "Accept-Encoding: gzip, deflate\r\n"); }

    // Content-Length
    if (config->body_len > 0 && !has_content_length) {
        p += sprintf(p, "Content-Length: %zu\r\n", config->body_len);
    }

    // Cookie (get from Isolate's Cookie Jar)
    if (isolate && xr_is_cookie_jar_enabled(isolate)) {
        XrCookieJar *jar = xr_get_cookie_jar(isolate);
        if (jar) {
            char *cookie_header =
                xr_cookie_jar_get_header(jar, url->host, url->path, url->is_https);
            if (cookie_header) {
                p += sprintf(p, "Cookie: %s\r\n", cookie_header);
                xr_free(cookie_header);
            }
        }
    }

    // Empty line
    *p++ = '\r';
    *p++ = '\n';

    // Body
    if (config->body && config->body_len > 0) {
        memcpy(p, config->body, config->body_len);
        p += config->body_len;
    }

    *out_len = p - buf;
    return buf;
}

// Get Location from response headers
static char *get_redirect_location(XrHttpResult *result) {
    for (int i = 0; i < result->header_count; i++) {
        if (result->headers[i].name_len == 8 &&
            strncasecmp(result->headers[i].name, "Location", 8) == 0) {
            return xr_strdup(result->headers[i].value);
        }
    }
    return NULL;
}

// Check if redirect status code
static bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Internal request function (single request, no redirect handling)
static XrHttpResult xr_http_request_internal(XrayIsolate *X, const XrHttpRequestConfig *config,
                                             const char *url_str);

/* ========== Main Request Function ========== */

XrHttpResult xr_http_request(XrayIsolate *X, const XrHttpRequestConfig *config) {
    XrHttpResult result;
    memset(&result, 0, sizeof(result));

    if (!config || !config->url) {
        result.error = XR_HTTP_ERR_URL_PARSE;
        return result;
    }

    // Default follow redirects, max 10 times
    bool follow_redirects = config->follow_redirects;
    int max_redirects = config->max_redirects > 0 ? config->max_redirects : 10;

    char *current_url = xr_strdup(config->url);
    int redirect_count = 0;

    while (current_url) {
        // Execute single request
        result = xr_http_request_internal(X, config, current_url);

        // Check if redirect needed
        if (follow_redirects && is_redirect_status(result.status_code) &&
            redirect_count < max_redirects) {
            char *location = get_redirect_location(&result);
            if (location) {
                // Handle relative URL
                char *new_url = NULL;
                if (location[0] == '/') {
                    // Relative path: extract scheme://host
                    XrHttpUrl parsed;
                    if (xr_http_url_parse(current_url, &parsed) == 0) {
                        size_t len = strlen(parsed.host) + strlen(location) + 16;
                        new_url = (char *) xr_malloc(len);
                        if (!new_url) {
                            xr_http_url_free(&parsed);
                            xr_free(location);
                            break;
                        }
                        snprintf(new_url, len, "%s://%s%s", parsed.is_https ? "https" : "http",
                                 parsed.host, location);
                        xr_http_url_free(&parsed);
                    }
                    xr_free(location);
                } else if (strncmp(location, "http://", 7) == 0 ||
                           strncmp(location, "https://", 8) == 0) {
                    // Absolute URL
                    new_url = location;
                } else {
                    xr_free(location);
                }

                if (new_url) {
                    // Free current result, continue redirect
                    xr_http_result_free(&result);
                    xr_free(current_url);
                    current_url = new_url;
                    redirect_count++;

                    // 303 redirect forces GET method
                    if (result.status_code == 303) {
                        XrHttpRequestConfig *mutable_config = (XrHttpRequestConfig *) config;
                        mutable_config->method = XR_HTTP_METHOD_GET;
                        mutable_config->body = NULL;
                        mutable_config->body_len = 0;
                    }
                    continue;
                }
            }
        }

        // No redirect needed or redirect complete
        break;
    }

    xr_free(current_url);
    return result;
}

// Internal request function implementation
static XrHttpResult xr_http_request_internal(XrayIsolate *X, const XrHttpRequestConfig *config,
                                             const char *url_str) {
    XrHttpResult result;
    memset(&result, 0, sizeof(result));

    // Parse URL
    XrHttpUrl url;
    if (xr_http_url_parse(url_str, &url) < 0) {
        result.error = XR_HTTP_ERR_URL_PARSE;
        result.error_msg = xr_strdup("Invalid URL");
        return result;
    }

    int timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : XR_HTTP_DEFAULT_TIMEOUT;
    (void) timeout_ms;
    char *request_buf = NULL;
    XrNetBuffer *recv_buf = NULL;
    XrPooledConn *pooled = NULL;
    XrConnPool *pool = NULL;
    bool conn_ok = false;  // Whether connection can be returned to pool

    // Resolve this Isolate's connection pool (lazy-init on first request).
    pool = http_client_pool(X);
    if (!pool) {
        result.error = XR_HTTP_ERR_MEMORY;
        result.error_msg = xr_strdup("HTTP context unavailable");
        goto cleanup;
    }

    // Try to get pooled connection (handles DNS, connect, TLS internally)
    pooled = xr_conn_pool_get(X, pool, url.host, (uint16_t) url.port, url.is_https);
    if (!pooled) {
        result.error = XR_HTTP_ERR_CONNECT;
        result.error_msg = xr_strdup("Connection failed");
        goto cleanup;
    }

    // Build request
    {
        size_t request_len;
        request_buf = build_request(X, config, &url, &request_len);
        if (!request_buf) {
            result.error = XR_HTTP_ERR_MEMORY;
            result.error_msg = xr_strdup("Memory allocation failed");
            goto cleanup;
        }

        // Send request via pooled connection (handles TCP/TLS transparently)
        size_t sent = 0;
        while (sent < request_len) {
            int n = xr_pooled_conn_write(X, pooled, request_buf + sent, request_len - sent);
            if (n <= 0) {
                result.error = XR_HTTP_ERR_SEND;
                result.error_msg = xr_strdup("Send failed");
                goto cleanup;
            }
            sent += n;
        }
    }

    // Receive response
    recv_buf = xr_netbuf_acquire(XR_HTTP_RECV_BUFFER_SIZE);
    if (!recv_buf) {
        result.error = XR_HTTP_ERR_MEMORY;
        result.error_msg = xr_strdup("Memory allocation failed");
        goto cleanup;
    }

    XrHttpParser parser;
    XrHttpResponse resp;
    xr_http_parser_init(&parser);
    xr_http_response_init(&resp);

    while (1) {
        // Ensure buffer has room for more data
        if (xr_netbuf_available(recv_buf) < XR_HTTP_RECV_BUFFER_SIZE) {
            if (recv_buf->capacity > 100 * 1024 * 1024) {  // 100MB limit
                result.error = XR_HTTP_ERR_TOO_LARGE;
                result.error_msg = xr_strdup("Response too large");
                goto cleanup;
            }
            if (!xr_netbuf_reserve(recv_buf, XR_HTTP_RECV_BUFFER_SIZE)) {
                result.error = XR_HTTP_ERR_MEMORY;
                result.error_msg = xr_strdup("Memory allocation failed");
                goto cleanup;
            }
        }

        // Receive data via pooled connection
        size_t avail = xr_netbuf_available(recv_buf);
        int n = xr_pooled_conn_read(X, pooled, recv_buf->bytes + recv_buf->size,
                                    avail > 0 ? avail - 1 : 0);
        if (n < 0) {
            result.error = XR_HTTP_ERR_RECV;
            result.error_msg = xr_strdup("Receive failed");
            goto cleanup;
        }

        if (n == 0) {
            // Connection closed by peer
            break;
        }

        xr_netbuf_advance(recv_buf, n);
        recv_buf->bytes[recv_buf->size] = '\0';

        // Try to parse response
        XrHttpParseResult parse_result =
            xr_http_parse_response(&parser, &resp, recv_buf->bytes, recv_buf->size);

        if (parse_result == XR_HTTP_PARSE_OK) {
            // Stream mode: return headers immediately, keep connection open
            if (config->stream) {
                result.status_code = resp.status_code;
                if (resp.status_text && resp.status_text_len > 0) {
                    result.status_text = (char *) xr_malloc(resp.status_text_len + 1);
                    if (result.status_text) {
                        memcpy(result.status_text, resp.status_text, resp.status_text_len);
                        result.status_text[resp.status_text_len] = '\0';
                    }
                }
                // Copy headers (compact single-allocation)
                copy_headers_compact(&result, resp.headers, resp.header_count);
                // Compact recv_buf: discard headers, keep leftover body data
                size_t leftover = recv_buf->size - resp.header_bytes;
                if (leftover > 0) {
                    memmove(recv_buf->bytes, recv_buf->bytes + resp.header_bytes, leftover);
                }
                recv_buf->size = leftover;

                result._stream_conn = pooled;
                result._stream_buf = recv_buf;
                result._stream_remaining = resp.content_length;  // -1 if unknown
                result._stream_chunked = resp.chunked;
                result.error = XR_HTTP_OK;
                // Prevent cleanup from closing conn / freeing buf
                pooled = NULL;
                recv_buf = NULL;
                goto cleanup;
            }

            // Check if receive complete
            if (resp.content_length >= 0) {
                size_t expected = resp.header_bytes + resp.content_length;
                if (recv_buf->size >= expected) {
                    break;  // Receive complete
                }
            } else if (!resp.chunked && !resp.keep_alive) {
                // No Content-Length, wait for connection close
                continue;
            }
        } else if (parse_result == XR_HTTP_PARSE_ERROR) {
            result.error = XR_HTTP_ERR_PARSE;
            result.error_msg = xr_strdup("Response parse error");
            goto cleanup;
        }
    }

    // Final parse
    XrHttpParseResult parse_result =
        xr_http_parse_response(&parser, &resp, recv_buf->bytes, recv_buf->size);
    if (parse_result == XR_HTTP_PARSE_ERROR) {
        result.error = XR_HTTP_ERR_PARSE;
        result.error_msg = xr_strdup("Response parse error");
        goto cleanup;
    }

    // Connection can be reused if server supports keep-alive
    conn_ok = resp.keep_alive;

    // Fill result
    result.status_code = resp.status_code;

    if (resp.status_text && resp.status_text_len > 0) {
        result.status_text = (char *) xr_malloc(resp.status_text_len + 1);
        if (result.status_text) {
            memcpy(result.status_text, resp.status_text, resp.status_text_len);
            result.status_text[resp.status_text_len] = '\0';
        }
    }

    // Copy Headers (compact single-allocation) and extract Set-Cookie
    if (resp.header_count > 0) {
        copy_headers_compact(&result, resp.headers, resp.header_count);

        // Collect Set-Cookie headers from the compacted copy
        const char *set_cookies[64];
        int set_cookie_count = 0;
        for (int i = 0; i < result.header_count; i++) {
            if (result.headers[i].name_len == 10 &&
                strncasecmp(result.headers[i].name, "Set-Cookie", 10) == 0) {
                if (set_cookie_count < 64) {
                    set_cookies[set_cookie_count++] = result.headers[i].value;
                }
            }
        }

        // Add Set-Cookie to Isolate's Cookie Jar
        if (set_cookie_count > 0 && X && xr_is_cookie_jar_enabled(X)) {
            XrCookieJar *jar = xr_get_cookie_jar(X);
            if (jar) {
                xr_cookie_jar_add_from_response(jar, set_cookies, set_cookie_count, url.host,
                                                url.path);
            }
        }
    }

    // Copy Body
    char *raw_body = NULL;
    size_t raw_body_len = 0;

    if (resp.body && resp.body_len > 0) {
        raw_body = (char *) resp.body;
        raw_body_len = resp.body_len;
    } else if (resp.content_length < 0 && recv_buf->size > (size_t) resp.header_bytes) {
        raw_body = recv_buf->bytes + resp.header_bytes;
        raw_body_len = recv_buf->size - resp.header_bytes;
    }

    // Handle chunked encoding
    char *decoded_body = NULL;
    if (resp.chunked && raw_body && raw_body_len > 0) {
        decoded_body = (char *) xr_malloc(raw_body_len + 1);
        if (!decoded_body)
            goto cleanup;
        memcpy(decoded_body, raw_body, raw_body_len);

        XrChunkedDecoder decoder;
        memset(&decoder, 0, sizeof(decoder));
        decoder.consume_trailer = true;

        size_t decoded_len = raw_body_len;
        ssize_t ret = xr_http_decode_chunked(&decoder, decoded_body, &decoded_len);

        if (ret >= 0 || ret == -2) {
            decoded_body[decoded_len] = '\0';
            raw_body = decoded_body;
            raw_body_len = decoded_len;
        } else {
            xr_free(decoded_body);
            decoded_body = NULL;
        }
    }

    if (raw_body && raw_body_len > 0) {
        // Check Content-Encoding for decompression
#if XR_HAS_COMPRESS
        XrContentEncoding compress_type = XR_CONTENT_ENC_NONE;
        for (size_t i = 0; i < resp.header_count; i++) {
            if (resp.headers[i].name_len == 16 &&
                strncasecmp(resp.headers[i].name, "Content-Encoding", 16) == 0) {
                // Extract value
                char encoding[64];
                size_t len = resp.headers[i].value_len < 63 ? resp.headers[i].value_len : 63;
                memcpy(encoding, resp.headers[i].value, len);
                encoding[len] = '\0';
                compress_type = xr_detect_content_encoding(encoding);
                break;
            }
        }

        if (compress_type != XR_CONTENT_ENC_NONE) {
            // Decompress data
            void *decompressed = NULL;
            size_t decompressed_len = 0;

            int dc_rc = -1;
            switch (compress_type) {
                case XR_CONTENT_ENC_GZIP:
                    dc_rc = xr_zlib_gzip_decompress(raw_body, raw_body_len, &decompressed,
                                                    &decompressed_len);
                    break;
                case XR_CONTENT_ENC_DEFLATE:
                    dc_rc = xr_zlib_deflate_decompress(raw_body, raw_body_len, &decompressed,
                                                       &decompressed_len);
                    break;
                default:
                    break;
            }
            if (dc_rc == 0) {
                result.body = (char *) decompressed;
                result.body_len = decompressed_len;
            } else {
                // Decompress failed, use original data
                result.body = (char *) xr_malloc(raw_body_len + 1);
                if (result.body) {
                    memcpy(result.body, raw_body, raw_body_len);
                    result.body[raw_body_len] = '\0';
                    result.body_len = raw_body_len;
                }
            }
        } else {
            // No compression, copy directly
            result.body = (char *) xr_malloc(raw_body_len + 1);
            if (result.body) {
                memcpy(result.body, raw_body, raw_body_len);
                result.body[raw_body_len] = '\0';
                result.body_len = raw_body_len;
            }
        }
#else
        // Compress module not available, copy body as-is
        result.body = (char *) xr_malloc(raw_body_len + 1);
        if (result.body) {
            memcpy(result.body, raw_body, raw_body_len);
            result.body[raw_body_len] = '\0';
            result.body_len = raw_body_len;
        }
#endif  // XR_HAS_COMPRESS
    }

    result.error = XR_HTTP_OK;

    // Free chunked decode buffer
    if (decoded_body)
        xr_free(decoded_body);

cleanup:
    // Return connection to pool (or close if error/no keep-alive). `pool` is
    // guaranteed non-NULL here because `pooled` is only set after a
    // successful http_client_pool() resolution.
    if (pooled) {
        if (result.error == XR_HTTP_OK && conn_ok) {
            xr_conn_pool_put(X, pool, pooled, url.host, (uint16_t) url.port, url.is_https, true);
        } else {
            xr_conn_pool_close(X, pool, pooled);
        }
    }
    if (request_buf)
        xr_free(request_buf);
    xr_netbuf_release(recv_buf);
    xr_http_url_free(&url);

    return result;
}

/* ========== Convenience Functions ========== */

XrHttpResult xr_http_get(XrayIsolate *X, const char *url) {
    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url;
    config.method = XR_HTTP_METHOD_GET;
    return xr_http_request(X, &config);
}

XrHttpResult xr_http_post(XrayIsolate *X, const char *url, const char *body, size_t body_len,
                          const char *content_type) {
    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url;
    config.method = XR_HTTP_METHOD_POST;
    config.body = body;
    config.body_len = body_len;

    XrHttpHeader headers[1];
    if (content_type) {
        headers[0].name = "Content-Type";
        headers[0].name_len = 12;
        headers[0].value = content_type;
        headers[0].value_len = strlen(content_type);
        config.headers = headers;
        config.header_count = 1;
    }

    return xr_http_request(X, &config);
}

XrHttpResult xr_http_put(XrayIsolate *X, const char *url, const char *body, size_t body_len,
                         const char *content_type) {
    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url;
    config.method = XR_HTTP_METHOD_PUT;
    config.body = body;
    config.body_len = body_len;

    XrHttpHeader headers[1];
    if (content_type) {
        headers[0].name = "Content-Type";
        headers[0].name_len = 12;
        headers[0].value = content_type;
        headers[0].value_len = strlen(content_type);
        config.headers = headers;
        config.header_count = 1;
    }

    return xr_http_request(X, &config);
}

XrHttpResult xr_http_delete(XrayIsolate *X, const char *url) {
    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url;
    config.method = XR_HTTP_METHOD_DELETE;
    return xr_http_request(X, &config);
}

/* ========== Request Context Implementation ========== */

XrHttpReqContext *xr_http_context_new(void) {
    XrHttpReqContext *ctx = (XrHttpReqContext *) xr_calloc(1, sizeof(XrHttpReqContext));
    return ctx;
}

void xr_http_context_set_timeout(XrHttpReqContext *ctx, int timeout_ms) {
    if (ctx)
        ctx->deadline_ms = timeout_ms;
}

void xr_http_context_cancel(XrHttpReqContext *ctx) {
    if (ctx)
        ctx->cancelled = true;
}

bool xr_http_context_is_cancelled(XrHttpReqContext *ctx) {
    return ctx && ctx->cancelled;
}

void xr_http_context_free(XrHttpReqContext *ctx) {
    xr_free(ctx);
}

/* ========== Streaming Response API ========== */

int xr_http_stream_read(XrayIsolate *X, XrHttpResult *result, char *buf, int max_bytes) {
    if (!result || !buf || max_bytes <= 0)
        return -1;
    if (!result->_stream_conn)
        return 0;  // already closed

    XrPooledConn *conn = (XrPooledConn *) result->_stream_conn;
    XrNetBuffer *nb = (XrNetBuffer *) result->_stream_buf;

    // First drain any leftover data in the recv buffer
    if (nb && nb->size > 0) {
        int copy = (int) nb->size;
        if (copy > max_bytes)
            copy = max_bytes;
        memcpy(buf, nb->bytes, copy);
        nb->size -= copy;
        if (nb->size > 0) {
            memmove(nb->bytes, nb->bytes + copy, nb->size);
        }
        if (result->_stream_remaining > 0) {
            result->_stream_remaining -= copy;
        }
        return copy;
    }

    // Check if body is fully consumed
    if (result->_stream_remaining == 0)
        return 0;

    // Read from connection
    int n = xr_pooled_conn_read(X, conn, buf, max_bytes);
    if (n <= 0)
        return n == 0 ? 0 : -1;

    if (result->_stream_remaining > 0) {
        result->_stream_remaining -= n;
        if (result->_stream_remaining < 0)
            result->_stream_remaining = 0;
    }
    return n;
}

void xr_http_stream_close(XrayIsolate *X, XrHttpResult *result) {
    if (!result)
        return;

    if (result->_stream_buf) {
        xr_netbuf_release((XrNetBuffer *) result->_stream_buf);
        result->_stream_buf = NULL;
    }
    if (result->_stream_conn) {
        XrPooledConn *conn = (XrPooledConn *) result->_stream_conn;
        // Don't return to pool — caller consumed partially, state unknown.
        xr_conn_pool_close(X, NULL, conn);
        result->_stream_conn = NULL;
    }
}
