/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http.h - HTTP module public interface
 *
 * KEY CONCEPT:
 *   Provides HTTP client and server functionality with support for
 *   HTTP/1.1, HTTP/2, cookies, proxy, and multipart forms.
 *   Each Isolate has its own XrHttpContext for multi-instance support.
 *
 * NOTE: WebSocket has been moved to separate 'ws' module.
 */

#ifndef XR_STDLIB_HTTP_H
#define XR_STDLIB_HTTP_H

#include "../../src/runtime/xisolate_internal.h"
#include "../../src/module/xmodule.h"
#include "http_server.h"
#include "http_multipart.h"
#include "http_cookie.h"
#include "http_proxy.h"
#include "http2_server.h"
#include "http2_client.h"
#include "../net/conn_pool.h"

// Per-Isolate HTTP module context, stored in module's native_handle
typedef struct XrHttpContext {
    /* === Server === */
    struct XrHttpServer *server;
    XrayIsolate *server_isolate;

    /* === Server Config (per-isolate) === */
    _Atomic int max_conns;              // Max connections (0 = unlimited)
    _Atomic int max_requests_per_conn;  // Max requests per connection (0 = unlimited)
    _Atomic int idle_timeout_ms;        // Idle timeout (ms)
    _Atomic int read_timeout_ms;        // Read timeout (ms)
    _Atomic int current_conns;          // Current connection count

    /* === Server Stats (atomic, lock-free) === */
    _Atomic uint64_t total_requests;        // Total requests served
    _Atomic uint64_t total_conns_accepted;  // Total connections accepted

    /* === Form Data === */
    XrFormData *form_data;

    /* === Cookie === */
    XrCookieJar *cookie_jar;
    bool cookie_jar_enabled;

    /* === Proxy === */
    XrProxyConfig *proxy;
    char **no_proxy;
    int no_proxy_count;

    /* === HTTP/2 === */
    XrH2Server *h2_server;
    struct XrH2Context *current_h2_ctx;  // Active request context (for h2Push)

    /* === Connection Pools (per-isolate) === */
    XrConnPool *conn_pool;     // TCP/TLS connection pool (net layer)
    XrH2Pool *h2_client_pool;  // HTTP/2 client connection pool

/* === Streaming Responses === */
#define XR_HTTP_MAX_STREAMS 16
    XrHttpResult *streams[XR_HTTP_MAX_STREAMS];  // Active stream slots (NULL = free)

} XrHttpContext;

// Get or create HTTP context for this Isolate
XR_FUNC XrHttpContext *xr_http_get_context(XrayIsolate *X);

// Free HTTP module context
XR_FUNC void xr_http_module_context_free(XrHttpContext *ctx);

// Load HTTP module
XR_FUNC XrModule *xr_load_module_http(XrayIsolate *isolate);

/* ========== Pure C Listen + Connection Handler (http_listen.c) ========== */

#include "../../src/coro/xyieldable.h"

// http.listen(port) -> bool (yieldable, accept loop + conn handler spawn)
XR_FUNC XrCFuncResult xr_http_listen_impl(XrayIsolate *X, XrValue *args, int nargs,
                                          XrValue *result);

// http.config(opts) -> void
XR_FUNC XrValue xr_http_config_impl(XrayIsolate *X, XrValue *args, int argc);

// http.response(status, body?, headers?) -> string
XR_FUNC XrValue xr_http_response_impl(XrayIsolate *X, XrValue *args, int argc);

// http.serverStats() -> Json { currentConns, totalRequests, totalConns }
XR_FUNC XrValue xr_http_server_stats(XrayIsolate *X, XrValue *args, int argc);

#endif  // XR_STDLIB_HTTP_H
