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
#include "http_server.h"
#include "http2_client.h"
#include "../net/conn_pool.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/runtime/xisolate_internal.h"

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

XrCFuncResult http_listen_impl(XrVMRuntime *X, XrValue *args, int nargs, XrValue *result);

#endif
