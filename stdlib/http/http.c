/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http.c - HTTP module implementation
 *
 * KEY CONCEPT:
 *   Binds HTTP client/server functionality to xray module system.
 *   Supports both synchronous calls and coroutine-based async I/O.
 *
 * WHY THIS DESIGN:
 *   - Request object pool reduces allocation pressure for high-throughput servers
 *   - TLS buffers avoid malloc/free per request
 *   - Yieldable C functions enable non-blocking I/O in coroutines
 */

#include "http_internal.h"
#include "../common.h"
#include "../../src/runtime/object/xjson_serde.h"
// NOTE: WebSocket moved to separate 'ws' module
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_closure.h"
#include "../../src/vm/xvm_coro_api.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xsocket.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/closure/xclosure.h"
#include "../../src/runtime/mem/xfixed_heap.h"
#include "../../src/base/xmalloc.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_net.h"

/* ========== External Declarations ========== */

extern XrValue h2_request(XrVMRuntime *X, XrValue *args, int argc);

/* ========== HTTP Server Implementation ========== */

/* ========== HTTP Context Management ========== */

// Get HTTP context (stored in module's native_handle)
XrHttpContext *http_get_context(XrVMRuntime *X) {
    if (!X || !X->module_registry)
        return NULL;

    // Get http module from registry
    XrModuleRegistry *registry = (XrModuleRegistry *) X->module_registry;
    XrModule *mod = NULL;
    if (registry->loaded_modules) {
        mod = (XrModule *) xr_hashmap_get(registry->loaded_modules, "http");
    }

    if (!mod)
        return NULL;

    // Get context from native_handle
    XrHttpContext *ctx = (XrHttpContext *) mod->native_handle;
    if (!ctx) {
        // First access, create context
        ctx = (XrHttpContext *) xr_calloc(1, sizeof(XrHttpContext));
        atomic_init(&ctx->current_conns, 0);

        mod->native_handle = ctx;
    }

    return ctx;
}

// Free HTTP module context.
static void http_context_destroy(void *handle) {
    XrHttpContext *ctx = (XrHttpContext *) handle;
    if (!ctx)
        return;

    // Free HTTP server
    if (ctx->server) {
        http_server_free(ctx->server);
        ctx->server = NULL;
    }

    // Free per-isolate HTTP connection pools
    if (ctx->http_conn_pool) {
        http_conn_pool_destroy(ctx->http_conn_pool);
        xr_free(ctx->http_conn_pool);
        ctx->http_conn_pool = NULL;
    }
    if (ctx->h2_client_pool) {
        http2_client_pool_destroy(ctx->h2_client_pool);
        ctx->h2_client_pool = NULL;
    }

    // NOTE: WebSocket connections are now managed by the separate 'ws' module

    xr_free(ctx);
}

// http.route(method, path, handler) - Register route
// handler: function or static string
static XrValue http_route(XrVMRuntime *X, XrValue *args, int argc) {
    XrHttpContext *ctx = http_get_context(X);
    if (argc < 3 || !ctx) {
        return xr_null();
    }

    // Auto-create global server instance
    if (!ctx->server) {
        ctx->server = http_server_new();
        if (!ctx->server) {
            fprintf(stderr, "http.route: failed to create server\n");
            return xr_null();
        }
    }

    // Get method
    size_t method_len;
    const char *method_str = xrs_string_arg(args[0], &method_len);
    if (!method_str)
        return xr_null();

    XrHttpMethod method = http_method_from_string(method_str, method_len);

    // Get path
    size_t path_len;
    const char *path = xrs_string_arg(args[1], &path_len);
    if (!path)
        return xr_null();

    // Copy path because router APIs expect a null-terminated C string.
    char *path_copy = (char *) xr_malloc(path_len + 1);
    if (!path_copy)
        return xr_null();
    memcpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';

    // The handler arg is polymorphic: closure / string / json. We dispatch
    // on the runtime tag and only allocate path_copy after a known shape
    // matches, so xr_value_is_closure is the right test here -- we don't
    // want xr_closure_from_arg's error path firing on the legitimate
    // string / json branches below.
    XrValue handler_arg = args[2];

    if (xr_value_is_closure(handler_arg)) {
        // Closure callback - register dynamic route
        XrClosure *closure = xr_value_to_closure(handler_arg);
        http_server_route(ctx->server, method, path_copy, closure);
        xr_free(path_copy);
    } else if (XR_IS_STRING(handler_arg)) {
        // Static string response - router owns its own body copy.
        size_t response_len;
        const char *response = xrs_string_arg(handler_arg, &response_len);
        if (response) {
            http_router_add_static(ctx->server->router, method, path_copy, response, response_len);
        }
        xr_free(path_copy);
    } else if (xr_value_is_json(handler_arg)) {
        // Json object - serialize in C layer and register as static response body.
        size_t json_len = 0;
        char *json_str = xr_json_stringify_to_cstr(X, handler_arg, &json_len);
        if (json_str && json_len > 0) {
            http_router_add_static(ctx->server->router, method, path_copy, json_str, json_len);
            xr_free(json_str);
            xr_free(path_copy);
        } else {
            // json_str was produced by xr_json_stringify_to_cstr which
            // allocates via xr_malloc; release through xr_free.
            xr_free(json_str);
            xr_free(path_copy);
        }
    } else {
        // Unsupported type
        xr_free(path_copy);
        fprintf(stderr, "http.route() handler must be a function, string, or Json\n");
        return xr_null();
    }

    return xr_null();
}

// http.ws(path, handler) - Register WebSocket upgrade route on HTTP server
// When a GET request with Upgrade:websocket hits this path, the connection
// is upgraded in-place and handler(wsConn) is called.
static XrValue http_ws_route(XrVMRuntime *X, XrValue *args, int argc) {
    XrHttpContext *ctx = http_get_context(X);
    if (argc < 2 || !ctx)
        return xr_null();

    if (!ctx->server) {
        ctx->server = http_server_new();
        if (!ctx->server)
            return xr_null();
    }

    size_t path_len;
    const char *path = xrs_string_arg(args[0], &path_len);
    if (!path)
        return xr_null();

    XrClosure *closure = xr_vm_closure_from_arg(X, args[1], "http.websocket");
    if (!closure)
        return xr_null();

    char *path_copy = (char *) xr_malloc(path_len + 1);
    if (!path_copy)
        return xr_null();
    memcpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';

    // Reserve a root slot before route insertion, then commit it only after
    // the router accepts the path. path_copy is only a null-terminated
    // scratch buffer; the router copies its own path segments.
    if (ctx->server->route_closure_count >= ctx->server->route_closure_capacity) {
        int new_cap =
            ctx->server->route_closure_capacity == 0 ? 16 : ctx->server->route_closure_capacity * 2;
        XrClosure **arr =
            (XrClosure **) xr_realloc(ctx->server->route_closures, new_cap * sizeof(XrClosure *));
        if (!arr) {
            xr_free(path_copy);
            return xr_null();
        }
        ctx->server->route_closures = arr;
        ctx->server->route_closure_capacity = new_cap;
    }

    bool ok = http_router_add_websocket(ctx->server->router, path_copy, (void *) closure);
    xr_free(path_copy);
    if (!ok)
        return xr_null();

    ctx->server->route_closures[ctx->server->route_closure_count++] = closure;
    return xr_null();
}

/* ========== http.listen migrated to http.xr ========== */

/*
 * C layer listen removed, using high-performance http.xr script implementation
 * Performance: C layer ~1.6K QPS vs script layer ~130K QPS
 */

// http.stopServer() -> void
static XrValue http_stop_server(XrVMRuntime *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrHttpContext *ctx = http_get_context(X);
    if (ctx && ctx->server) {
        http_server_stop(ctx->server);
    }

    return xr_null();
}

#define XR_STDLIB_VM_BIND_MODULE_HTTP 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_HTTP

XR_FUNC XrModule *xr_load_module_http(XrVMRuntime *isolate) {
    // 1. Create Native module
    XrModule *mod = xr_module_create_native(isolate, "http");
    if (!mod)
        return NULL;
    mod->native_handle_destroy = http_context_destroy;

    xr_stdlib_vm_bind_http_generated(isolate, mod);

    // 3. Mark as loaded
    mod->requires_script = true;
    mod->loaded = true;
    return mod;
}
