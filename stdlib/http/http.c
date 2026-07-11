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
 *   Binds HTTP native data-plane functionality to xray module system.
 *
 * WHY THIS DESIGN:
 *   - HTTP/1.x request semantics live in pure Xray stdlib/http/http.xr
 *   - Native code remains only for HTTP/2 and internal XPkg/client data planes
 *   - Per-Isolate pools avoid cross-runtime connection lifetime coupling
 */

#include "http_internal.h"
#include "../common.h"
#include "../../src/module/xmodule.h"
#include "../../src/base/xmalloc.h"

/* ========== External Declarations ========== */

extern XrValue h2_request(XrVMRuntime *X, XrValue *args, int argc);

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

        mod->native_handle = ctx;
    }

    return ctx;
}

// Free HTTP module context.
static void http_context_destroy(void *handle) {
    XrHttpContext *ctx = (XrHttpContext *) handle;
    if (!ctx)
        return;

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

    xr_free(ctx);
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
