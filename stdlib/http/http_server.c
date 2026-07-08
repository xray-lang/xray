/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_server.c - GMP model HTTP server implementation
 *
 * KEY CONCEPT:
 *   1. listen() creates Listener coroutine
 *   2. Listener coroutine loops accept(), creating Conn coroutine per connection
 *   3. Conn coroutine handles request/response, closes connection when done
 *   4. All I/O via xsocket API, netpoll handles blocked coroutines
 */

#include "../../src/base/xmalloc.h"
#include "http_server.h"

#include "../../src/os/os_net.h"

/* ========== Server API ========== */

/*
 * Create server
 */
XrHttpServer *xr_http_server_new(struct XrVMRuntime *isolate) {
    XrHttpServer *server = (XrHttpServer *) xr_calloc(1, sizeof(XrHttpServer));
    if (!server)
        return NULL;

    server->isolate = isolate;
    server->listen_fd = -1;
    server->running = false;

    // Create router
    server->router = xr_router_new();

    return server;
}

/*
 * Free server
 */
void xr_http_server_free(XrHttpServer *server) {
    if (!server)
        return;

    if (server->listen_fd >= 0) {
        xr_closesocket(server->listen_fd);
    }

    if (server->router) {
        xr_router_free(server->router);
    }

    // Free route closure array (closures themselves managed by GC)
    if (server->route_closures) {
        xr_free(server->route_closures);
    }

    xr_free(server);
}

/*
 * Add route
 */
void xr_http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                          struct XrClosure *handler) {
    if (!server || !server->router || !path || !handler)
        return;

    // Save closure to array (prevent GC collection)
    if (server->route_closure_count >= server->route_closure_capacity) {
        int new_cap = server->route_closure_capacity == 0 ? 16 : server->route_closure_capacity * 2;
        struct XrClosure **new_arr = (struct XrClosure **) xr_realloc(
            server->route_closures, new_cap * sizeof(struct XrClosure *));
        if (!new_arr)
            return;
        server->route_closures = new_arr;
        server->route_closure_capacity = new_cap;
    }
    server->route_closures[server->route_closure_count++] = handler;

    // Register to router (closure stored in user_data)
    xr_router_add(server->router, method, path, (XrRouteHandler) 1, (void *) handler);
}

/*
 * Add static response route
 */
void xr_http_server_static(XrHttpServer *server, XrHttpMethod method, const char *path,
                           const char *response, size_t response_len) {
    if (!server || !server->router || !path || !response)
        return;
    xr_router_add_static(server->router, method, path, response, response_len);
}

/*
 * Try to find a prebuilt response for raw HTTP data.
 * Quick-parses method+path, does route lookup, returns prebuilt pointer.
 * Does NOT perform I/O - caller writes via coroutine-safe path.
 */
bool xr_http_try_prebuilt(XrRouter *router, const char *raw_data, size_t data_len,
                          const char **out_resp, size_t *out_len) {
    if (!router || !raw_data || data_len < 14)
        return false;

    // Quick parse method (only GET for prebuilt fast path)
    const char *p = raw_data;
    const char *end = raw_data + data_len;
    XrHttpMethod method;

    if (p[0] == 'G' && p[1] == 'E' && p[2] == 'T' && p[3] == ' ') {
        method = XR_HTTP_METHOD_GET;
        p += 4;
    } else {
        return false;
    }

    // Quick parse path (stop at space or '?')
    const char *path = p;
    while (p < end && *p != ' ' && *p != '?')
        p++;
    size_t path_len = p - path;
    if (path_len == 0)
        return false;

    // Route lookup (stack-only, zero GC allocation)
    XrRouteParams params;
    params.count = 0;
    void *user_data = NULL;
    const char *static_resp = NULL, *prebuilt_resp = NULL;
    size_t static_len = 0, prebuilt_len = 0;

    xr_router_find(router, method, path, path_len, &params, &user_data, &static_resp, &static_len,
                   &prebuilt_resp, &prebuilt_len);

    if (!prebuilt_resp || prebuilt_len == 0 || params.count > 0)
        return false;

    *out_resp = prebuilt_resp;
    *out_len = prebuilt_len;
    return true;
}

/*
 * Stop server
 */
void xr_http_server_stop(XrHttpServer *server) {
    if (!server)
        return;

    server->running = false;

    // Close listen socket, wake up accept
    if (server->listen_fd >= 0) {
        xr_closesocket(server->listen_fd);
        server->listen_fd = -1;
    }
}
