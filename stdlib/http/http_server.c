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
XrHttpServer *xr_http_server_new(void) {
    XrHttpServer *server = (XrHttpServer *) xr_calloc(1, sizeof(XrHttpServer));
    if (!server)
        return NULL;

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
    xr_router_add(server->router, method, path, (void *) handler);
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
