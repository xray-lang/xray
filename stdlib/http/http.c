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

#include "http.h"
#include "http_client.h"
#include "http_parser.h"
#include "http_router.h"
#include "http_server.h"
#include "../../src/base/xplatform.h"
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
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"
#include <ctype.h>
#include "../../src/runtime/closure/xclosure.h"
#include "../../src/runtime/mem/xfixed_heap.h"
#include "../../src/base/xmalloc.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_net.h"

/* ========== External Declarations ========== */

extern XrValue xr_string_value(XrString *str);
extern XrString *xr_string_intern(XrVMRuntime *X, const char *str, size_t len, uint32_t hash);
struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrVMRuntime *X);
extern XrArray *xr_array_new(struct XrCoroutine *coro);
extern void xr_array_push(XrArray *arr, XrValue value);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue h2_request(XrVMRuntime *X, XrValue *args, int argc);

/* ========== URL Copy Optimization ========== */

// 99% of URLs are < 2KB, use stack allocation to avoid malloc/free
#define URL_STACK_SIZE 2048

// URL copy macros: small URLs use stack buffer, large ones fall back to malloc
#define URL_COPY_BEGIN(url, url_len)                                                               \
    char _url_stack_buf[URL_STACK_SIZE];                                                           \
    char *url_copy;                                                                                \
    bool _url_need_free = false;                                                                   \
    if ((url_len) < URL_STACK_SIZE) {                                                              \
        url_copy = _url_stack_buf;                                                                 \
    } else {                                                                                       \
        url_copy = (char *) xr_malloc((url_len) + 1);                                              \
        if (!url_copy)                                                                             \
            return xr_null();                                                                      \
        _url_need_free = true;                                                                     \
    }                                                                                              \
    memcpy(url_copy, (url), (url_len));                                                            \
    url_copy[(url_len)] = '\0';

#define URL_COPY_END()                                                                             \
    if (_url_need_free)                                                                            \
    xr_free(url_copy)

/* ========== Helper Functions ========== */

// Get string field from Json
static const char *get_json_string(XrVMRuntime *X, XrJson *json, const char *key, size_t *out_len) {
    XrValue val = xr_json_get_by_key(X, json, key);

    if (!XR_IS_STRING(val)) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }

    XrString *s = XR_TO_STRING(val);
    if (out_len)
        *out_len = s->length;
    return s->data;
}

// Get int field from Json
static int64_t get_json_int(XrVMRuntime *X, XrJson *json, const char *key, int64_t default_val) {
    XrValue val = xr_json_get_by_key(X, json, key);

    if (XR_IS_INT(val))
        return XR_TO_INT(val);
    if (XR_IS_FLOAT(val))
        return (int64_t) XR_TO_FLOAT(val);
    return default_val;
}

static char *normalize_http_method_token(const char *method, size_t method_len) {
    if (!method)
        return NULL;

    size_t start = 0;
    while (start < method_len && (method[start] == ' ' || method[start] == '\t'))
        start++;
    size_t end = method_len;
    while (end > start && (method[end - 1] == ' ' || method[end - 1] == '\t'))
        end--;
    if (end <= start)
        return NULL;

    size_t len = end - start;
    char *out = (char *) xr_malloc(len + 1);
    if (!out)
        return NULL;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) method[start + i];
        if (c <= 32 || c == 127) {
            xr_free(out);
            return NULL;
        }
        out[i] = (char) toupper(c);
    }
    out[len] = '\0';
    return out;
}

// Convert HTTP result to xray Json
static XrValue result_to_json(XrVMRuntime *X, XrHttpResult *result) {
    // Create response object (dictionary mode for flexible field names)
    XrJson *json = xr_json_new(xr_current_coro(X));

    // status
    xr_json_set_by_key(X, json, "status", xr_int(result->status_code));

    // statusText — always present so callers can rely on the field existing.
    xr_json_set_by_key(X, json, "statusText",
                       result->status_text ? xrs_string_value_c(X, result->status_text)
                                           : xrs_string_value_c(X, ""));

    // headers - Shape transition for flexible header names
    XrJson *headers_json = xr_json_new(xr_current_coro(X));
    for (int i = 0; i < result->header_count; i++) {
        XrHttpHeader *h = &result->headers[i];
        // Header name needs null-termination
        char *header_name = (char *) xr_malloc(h->name_len + 1);
        if (!header_name)
            break;
        memcpy(header_name, h->name, h->name_len);
        header_name[h->name_len] = '\0';

        xr_json_set_by_key(X, headers_json, header_name,
                           xrs_string_value_n(X, h->value, h->value_len));
        xr_free(header_name);
    }
    xr_json_set_by_key(X, json, "headers", xr_json_value(headers_json));

    // body
    if (result->body && result->body_len > 0) {
        xr_json_set_by_key(X, json, "body", xrs_string_value_n(X, result->body, result->body_len));
    } else {
        xr_json_set_by_key(X, json, "body", xrs_string_value_c(X, ""));
    }

    // error
    if (result->error != XR_HTTP_OK) {
        xr_json_set_by_key(X, json, "error",
                           xrs_string_value_c(X, xr_http_error_string(result->error)));
    } else {
        xr_json_set_by_key(X, json, "error", xr_null());
    }

    // ok
    xr_json_set_by_key(X, json, "ok",
                       xr_bool(result->error == XR_HTTP_OK && result->status_code >= 200 &&
                               result->status_code < 300));

    return xr_json_value(json);
}

// http.request(options: Json) -> Response
// options: url (required), method, body, headers, timeout (ms)
static XrValue http_request(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1) {
        fprintf(stderr, "http.request() requires 1 argument\n");
        return xr_null();
    }

    // Check argument type: must be Json
    if (!xr_value_is_json(args[0])) {
        fprintf(stderr, "http.request() argument must be a Json object\n");
        return xr_null();
    }

    XrJson *options = xr_value_to_json(args[0]);

    // Get URL
    size_t url_len;
    const char *url = get_json_string(X, options, "url", &url_len);
    if (!url || url_len == 0) {
        fprintf(stderr, "http.request() requires 'url' field\n");
        return xr_null();
    }

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)

    // Initialize config
    XrHttpRequestConfig config;
    xr_http_request_config_init(&config);
    config.url = url_copy;
    char *method_name = NULL;

    // Get method
    size_t method_len;
    const char *method = get_json_string(X, options, "method", &method_len);
    if (method && method_len > 0) {
        method_name = normalize_http_method_token(method, method_len);
        if (!method_name) {
            XrHttpResult result;
            memset(&result, 0, sizeof(result));
            result.error = XR_HTTP_ERR_PARSE;
            result.error_msg = xr_strdup("Invalid HTTP method");
            XrValue ret = result_to_json(X, &result);
            xr_http_result_free(&result);
            URL_COPY_END();
            return ret;
        }
        config.method_name = method_name;
        config.method_name_len = strlen(method_name);
        config.method = xr_http_method_from_string(method_name, config.method_name_len);
    }

    // Get body
    size_t body_len;
    const char *body = get_json_string(X, options, "body", &body_len);
    if (body && body_len > 0) {
        config.body = body;
        config.body_len = body_len;
    }

    // Get timeout
    config.timeout_ms = (int) get_json_int(X, options, "timeout", XR_HTTP_DEFAULT_TIMEOUT);

    // Get headers (supports Json, Dictionary, and Map types)
    XrValue headers_val = xr_json_get_by_key(X, options, "headers");

    XrHttpHeader *custom_headers = NULL;
    int custom_header_count = 0;

    if (xr_value_is_map(headers_val)) {
        // Map type: iterate Map directly
        XrMap *headers_map = xr_value_to_map(headers_val);
        if (headers_map) {
            custom_header_count = headers_map->count;
            if (custom_header_count > 0) {
                custom_headers =
                    (XrHttpHeader *) xr_malloc(sizeof(XrHttpHeader) * custom_header_count);
                if (!custom_headers) {
                    URL_COPY_END();
                    xr_free(method_name);
                    return xr_null();
                }

                int idx = 0;
                uint32_t map_size = headers_map->nentries;
                for (uint32_t i = 0; i < map_size && idx < custom_header_count; i++) {
                    XrMapEntry *node = &headers_map->entries[i];
                    if (!XR_MAP_ENTRY_EMPTY(node) && XR_IS_STRING(node->key) &&
                        XR_IS_STRING(node->value)) {
                        XrString *k = XR_TO_STRING(node->key);
                        XrString *v = XR_TO_STRING(node->value);
                        custom_headers[idx].name = k->data;
                        custom_headers[idx].name_len = k->length;
                        custom_headers[idx].value = v->data;
                        custom_headers[idx].value_len = v->length;
                        idx++;
                    }
                }
                custom_header_count = idx;
            }
        }
        config.headers = custom_headers;
        config.header_count = custom_header_count;
    } else if (xr_value_is_json(headers_val)) {
        XrJson *headers_json = xr_value_to_json(headers_val);
        XrClass *cls = headers_json->klass;

        if (cls && cls->field_count > 0) {
            custom_header_count = cls->field_count;
            custom_headers = (XrHttpHeader *) xr_malloc(sizeof(XrHttpHeader) * custom_header_count);
            if (!custom_headers) {
                URL_COPY_END();
                xr_free(method_name);
                return xr_null();
            }

            int idx = 0;
            for (uint16_t i = 0; i < cls->field_count; i++) {
                XrValue val = xr_instance_get_dynamic_field(headers_json, i);

                const char *field_name = cls->fields[i].name;
                if (field_name && XR_IS_STRING(val)) {
                    XrString *v = XR_TO_STRING(val);
                    custom_headers[idx].name = field_name;
                    custom_headers[idx].name_len = strlen(field_name);
                    custom_headers[idx].value = v->data;
                    custom_headers[idx].value_len = v->length;
                    idx++;
                }
            }
            custom_header_count = idx;
        }

        config.headers = custom_headers;
        config.header_count = custom_header_count;
    }

    XrHttpResult result = xr_http_request(X, &config);
    XrValue ret = result_to_json(X, &result);
    xr_http_result_free(&result);

    // Cleanup
    URL_COPY_END();
    if (custom_headers)
        xr_free(custom_headers);
    xr_free(method_name);

    return ret;
}

/* ========== HTTP Server Implementation ========== */

/* ========== HTTP Context Management ========== */

// Get HTTP context (stored in module's native_handle)
XrHttpContext *xr_http_get_context(XrVMRuntime *X) {
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

// Free HTTP module context
void xr_http_module_context_free(XrHttpContext *ctx) {
    if (!ctx)
        return;

    // Free HTTP server
    if (ctx->server) {
        xr_http_server_free(ctx->server);
        ctx->server = NULL;
    }

    // Free per-isolate connection pools
    if (ctx->conn_pool) {
        xr_conn_pool_destroy(ctx->conn_pool);
        xr_free(ctx->conn_pool);
        ctx->conn_pool = NULL;
    }
    if (ctx->h2_client_pool) {
        xr_h2_pool_destroy(ctx->h2_client_pool);
        ctx->h2_client_pool = NULL;
    }

    // NOTE: WebSocket connections are now managed by the separate 'ws' module

    xr_free(ctx);
}

// http.route(method, path, handler) - Register route
// handler: function or static string
static XrValue http_route(XrVMRuntime *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 3 || !ctx) {
        return xr_null();
    }

    // Auto-create global server instance
    if (!ctx->server) {
        ctx->server = xr_http_server_new();
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

    XrHttpMethod method = xr_http_method_from_string(method_str, method_len);

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
        xr_http_server_route(ctx->server, method, path_copy, closure);
        xr_free(path_copy);
    } else if (XR_IS_STRING(handler_arg)) {
        // Static string response - router owns its own body copy.
        size_t response_len;
        const char *response = xrs_string_arg(handler_arg, &response_len);
        if (response) {
            xr_router_add_static(ctx->server->router, method, path_copy, response, response_len);
        }
        xr_free(path_copy);
    } else if (xr_value_is_json(handler_arg)) {
        // Json object - serialize in C layer and register as static response body.
        size_t json_len = 0;
        char *json_str = xr_json_stringify_to_cstr(X, handler_arg, &json_len);
        if (json_str && json_len > 0) {
            xr_router_add_static(ctx->server->router, method, path_copy, json_str, json_len);
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
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 2 || !ctx)
        return xr_null();

    if (!ctx->server) {
        ctx->server = xr_http_server_new();
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

    // Save closure to server's root array (prevent GC collection)
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
    ctx->server->route_closures[ctx->server->route_closure_count++] = closure;

    xr_router_add_websocket(ctx->server->router, path_copy, (void *) closure);
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

    XrHttpContext *ctx = xr_http_get_context(X);
    if (ctx && ctx->server) {
        xr_http_server_stop(ctx->server);
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

    xr_stdlib_vm_bind_http_generated(isolate, mod);

    // 3. Mark as loaded
    mod->requires_script = true;
    mod->loaded = true;
    return mod;
}
