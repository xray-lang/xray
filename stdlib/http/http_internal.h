/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_internal.h - Private HTTP native module boundary
 */

#ifndef XR_STDLIB_HTTP_INTERNAL_H
#define XR_STDLIB_HTTP_INTERNAL_H

#include "http.h"
#include "http_router.h"
#include "http2_client.h"
#include "../net/conn_pool.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/xisolate_internal.h"

struct XrClosure;

typedef struct XrHttpServer {
    int listen_fd;
    volatile bool running;

    XrRouter *router;

    struct XrClosure **route_closures;
    int route_closure_count;
    int route_closure_capacity;
} XrHttpServer;

// Per-Isolate HTTP module context, stored in module's native_handle.
typedef struct XrHttpContext {
    /* === Server === */
    struct XrHttpServer *server;
    XrVMRuntime *server_isolate;

    /* === Server Runtime State === */
    _Atomic int current_conns;

    /* === Connection Pools (per-isolate) === */
    XrConnPool *conn_pool;
    XrH2Pool *h2_client_pool;

} XrHttpContext;

XrHttpContext *http_get_context(XrVMRuntime *X);

XrHttpServer *http_server_new(void);
void http_server_free(XrHttpServer *server);
void http_server_route(XrHttpServer *server, XrHttpMethod method, const char *path,
                       struct XrClosure *handler);
void http_server_stop(XrHttpServer *server);

XrCFuncResult http_listen_impl(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result);

#endif
