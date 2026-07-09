/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_server.h - HTTP server with per-connection coroutine model
 *
 * KEY CONCEPT:
 *   Per-connection coroutine design. Blocking I/O handled by netpoll,
 *   no event loop needed. Handler can freely use go/await/channel.
 */

#ifndef XR_STDLIB_HTTP_SERVER_H
#define XR_STDLIB_HTTP_SERVER_H

#include "../../src/base/xdefs.h"
#include <stdbool.h>
#include "http_router.h"

/* ========== Constants ========== */

#define XR_HTTP_MAX_HEADER_SIZE 8192
#define XR_HTTP_MAX_BODY_SIZE 1048576
#define XR_HTTP_BACKLOG 1024

/* ========== Forward Declarations ========== */

struct XrClosure;

/* ========== HTTP Server ========== */

typedef struct XrHttpServer {
    int listen_fd;
    volatile bool running;

    // Router
    XrRouter *router;

    // Route closures storage (prevent GC collection)
    struct XrClosure **route_closures;
    int route_closure_count;
    int route_closure_capacity;

} XrHttpServer;

/* ========== Server API ========== */

// Create server
XrHttpServer *http_server_new(void);

// Free server
void http_server_free(XrHttpServer *server);

// Add route (handler is xray closure)
void http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                       struct XrClosure *handler);

// Stop server
void http_server_stop(XrHttpServer *server);

#endif  // XR_STDLIB_HTTP_SERVER_H
