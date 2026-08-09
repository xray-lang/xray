/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_http_url.c - Shared HTTP transport primitives.
 *
 * KEY CONCEPT:
 *   URL authority parsing (RFC 3986, IPv6 literal support) and the
 *   redirect-status predicate, shared by the HTTP/2 transport and the stdlib
 *   http redirect-status migration oracle. The HTTP/1.x request path is owned
 *   by pure-Xray stdlib/http/http.xr and no longer needs these in C.
 */

#include "xr_http_url.h"
#include "../base/xmalloc.h"
#include <string.h>

int http_url_parse(const char *url, XrHttpUrl *out) {
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

        if (strcmp(out->scheme, "http") != 0 && strcmp(out->scheme, "https") != 0) {
            http_url_free(out);
            return -1;
        }

        out->is_https = (strcmp(out->scheme, "https") == 0);
        out->port = out->is_https ? XR_HTTP_DEFAULT_HTTPS_PORT : XR_HTTP_DEFAULT_PORT;

        p = scheme_end + 3;
    }

    // Reject URL userinfo (`user:pass@host`). The HTTP API does not derive
    // Authorization from URLs, so accepting and stripping credentials would
    // silently turn an authenticated-looking URL into an unauthenticated request.
    {
        const char *scan = p;
        while (*scan && *scan != '/' && *scan != '?' && *scan != '#') {
            if (*scan == '@') {
                http_url_free(out);
                return -1;
            }
            scan++;
        }
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
            http_url_free(out);
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
        http_url_free(out);
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
            http_url_free(out);
            return -1;
        }
        if (port <= 0 || port > 65535) {
            http_url_free(out);
            return -1;
        }
        out->port = port;
    }

    // Parse path (including query string)
    if (*p) {
        out->path = xr_strdup(p);
    } else {
        out->path = xr_strdup("/");
    }

    return 0;
}

void http_url_free(XrHttpUrl *url) {
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

bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}
