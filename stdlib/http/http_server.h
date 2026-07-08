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
XR_FUNC XrHttpServer *xr_http_server_new(void);

// Free server
XR_FUNC void xr_http_server_free(XrHttpServer *server);

// Add route (handler is xray closure)
XR_FUNC void xr_http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                                  struct XrClosure *handler);

// Add static response route
XR_FUNC void xr_http_server_static(XrHttpServer *server, XrHttpMethod method, const char *path,
                                   const char *response, size_t response_len);

// Stop server
XR_FUNC void xr_http_server_stop(XrHttpServer *server);

/* ========== Internal Functions ========== */

/*
 * Try to find a prebuilt response for raw HTTP data.
 * Quick-parses method+path (no full header parsing), does route lookup.
 * Returns prebuilt response pointer via out params, or NULL if not prebuilt.
 *
 * WHY THIS DESIGN:
 *   Separates route lookup from I/O. The caller writes the response via
 *   coroutine-safe net.writeFast, avoiding blocking worker threads.
 *   Zero GC allocation: only stack variables used for route lookup.
 */
XR_FUNC bool xr_http_try_prebuilt(XrRouter *router, const char *raw_data, size_t data_len,
                                  const char **out_resp, size_t *out_len);

#endif  // XR_STDLIB_HTTP_SERVER_H
