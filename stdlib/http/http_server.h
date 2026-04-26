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
#include <stdint.h>
#include <stdbool.h>
#include "http_parser.h"
#include "http_router.h"

/* ========== Constants ========== */

#define XR_HTTP_MAX_HEADER_SIZE 8192
#define XR_HTTP_MAX_BODY_SIZE 1048576
#define XR_HTTP_BACKLOG 1024

/* ========== Forward Declarations ========== */

struct XrayIsolate;
struct XrCoroutine;
struct XrClosure;

/* ========== HTTP Request ========== */

typedef struct XrHttpReq {
    XrHttpMethod method;
    char *path;
    char *query;
    char *body;
    size_t body_len;
    XrRouteParams params;

    // Connection info
    int fd;
    bool keep_alive;
} XrHttpReq;

/* ========== HTTP Response ========== */

typedef struct XrHttpResp {
    int status;
    const char *content_type;
    char *body;
    size_t body_len;
    bool body_owned;
} XrHttpResp;

/* ========== HTTP Server ========== */

struct XrWebSocket;
typedef void (*XrWsConnectionHandler)(struct XrayIsolate *X, struct XrWebSocket *ws,
                                      void *user_data);

typedef struct XrHttpServer {
    int listen_fd;
    uint16_t port;
    volatile bool running;

    // Router
    XrRouter *router;

    // VM instance
    struct XrayIsolate *isolate;

    // Connection handler closure (for creating connection coroutines)
    struct XrClosure *conn_handler_closure;

    // Route closures storage (prevent GC collection)
    struct XrClosure **route_closures;
    int route_closure_count;
    int route_closure_capacity;

    // GC that owns route closures (the coroutine that called http.route)
    struct XrCoroGC *owner_gc;

    // WebSocket handler
    XrWsConnectionHandler ws_handler;
    void *ws_user_data;

    // Stats
    uint64_t total_requests;
    uint64_t total_connections;
    uint64_t active_connections;

    // State machine (for yieldable protocol support)
    void *listener_state;
} XrHttpServer;

/* ========== Server API ========== */

// Create server
XR_FUNC XrHttpServer *xr_http_server_new(struct XrayIsolate *isolate);

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

// Set WebSocket handler
XR_FUNC void xr_http_server_set_ws_handler(XrHttpServer *server, XrWsConnectionHandler handler,
                                           void *user_data);

/* ========== Internal Functions ========== */

// Read and parse HTTP request
XR_FUNC int xr_http_read_request(struct XrayIsolate *X, int fd, XrHttpReq *req, char *buf,
                                 size_t buf_size);

// Send HTTP response
XR_FUNC int xr_http_write_response(struct XrayIsolate *X, int fd, XrHttpResp *resp);

// Send simple text response
XR_FUNC int xr_http_send_text(struct XrayIsolate *X, int fd, int status, const char *body);

// Send error response
XR_FUNC int xr_http_send_error(struct XrayIsolate *X, int fd, int status, const char *message);

// Send redirect response (301/302)
XR_FUNC int xr_http_send_redirect(struct XrayIsolate *X, int fd, int status, const char *location);

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
