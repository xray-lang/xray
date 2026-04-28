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
 *   - Request object pool reduces GC pressure for high-throughput servers
 *   - TLS buffers avoid malloc/free per request
 *   - Yieldable C functions enable non-blocking I/O in coroutines
 */

#include "http.h"
#include "http_client.h"
#include "http_parser.h"
#include "http_router.h"
#include "http_server.h"
#include "../../src/base/xplatform.h"
#include "http_stream.h"
#include "http_multipart.h"
#include "http_proxy.h"
#include "../json/json.h"
#include "../common.h"
// NOTE: WebSocket moved to separate 'ws' module
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/coro/xyieldable.h"
#include "../../src/coro/xworker.h"
#include "../../src/coro/xsocket.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/gc/xgc_internal.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/os/os_net.h"

/* ========== External Declarations ========== */

// C function coroutine creation (supports Yieldable I/O)
extern XrCoroutine *xr_coro_create_cfunc(XrayIsolate *X,
                                         XrCFuncResult (*cfunc)(XrayIsolate *, XrValue *, int,
                                                                XrValue *),
                                         XrValue *args, int argc, const char *name);

// Closure check macro (XR_TFUNCTION is closure type)
#define XR_IS_CLOSURE(v)                                                                           \
    (XR_IS_PTR(v) && XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(v)) == XR_TFUNCTION)

extern XrValue xr_string_value(XrString *str);
extern XrString *xr_string_intern(XrayIsolate *X, const char *str, size_t len, uint32_t hash);
struct XrCoroutine;
extern struct XrCoroutine *xr_current_coro(XrayIsolate *X);
extern XrArray *xr_array_new(struct XrCoroutine *coro);
extern void xr_array_push(XrArray *arr, XrValue value);
extern XrValue xr_value_from_array(XrArray *arr);

/* ========== Server Config and Connection Limits ========== */

// Server config defaults
#include <stdatomic.h>

#define HTTP_DEFAULT_MAX_CONNS 10000
#define HTTP_DEFAULT_IDLE_TIMEOUT_MS 60000
#define HTTP_DEFAULT_READ_TIMEOUT_MS 30000

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
static const char *get_json_string(XrayIsolate *X, XrJson *json, const char *key, size_t *out_len) {
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
static int64_t get_json_int(XrayIsolate *X, XrJson *json, const char *key, int64_t default_val) {
    XrValue val = xr_json_get_by_key(X, json, key);

    if (XR_IS_INT(val))
        return XR_TO_INT(val);
    if (XR_IS_FLOAT(val))
        return (int64_t) XR_TO_FLOAT(val);
    return default_val;
}

// Convert HTTP result to xray Json
static XrValue result_to_json(XrayIsolate *X, XrHttpResult *result) {
    // Create response object (dictionary mode for flexible field names)
    XrJson *json = xr_json_new(xr_current_coro(X), 8);

    // status
    xr_json_set_by_key(X, json, "status", xr_int(result->status_code));

    // statusText
    if (result->status_text) {
        xr_json_set_by_key(X, json, "statusText", xrs_string_value_c(X, result->status_text));
    }

    // headers - Shape transition for flexible header names
    XrJson *headers_json = xr_json_new(xr_current_coro(X), 16);
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

/* ========== HTTP Method Implementations ========== */

// http.get(url: string) -> Response
static XrValue http_get(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        fprintf(stderr, "http.get() requires 1 argument\n");
        return xr_null();
    }

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url) {
        fprintf(stderr, "http.get() argument must be a string\n");
        return xr_null();
    }

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)

    XrValue ret;

    // Synchronous call
    XrHttpResult result = xr_http_get(X, url_copy);
    ret = result_to_json(X, &result);
    xr_http_result_free(&result);

    URL_COPY_END();
    return ret;
}

// http.post(url: string, body?: string, contentType?: string) -> Response
static XrValue http_post(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        fprintf(stderr, "http.post() requires at least 1 argument\n");
        return xr_null();
    }

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url) {
        fprintf(stderr, "http.post() first argument must be a string\n");
        return xr_null();
    }

    const char *body = NULL;
    size_t body_len = 0;
    if (argc >= 2 && XR_IS_STRING(args[1])) {
        body = xrs_string_arg(args[1], &body_len);
    }

    const char *content_type = "application/json";
    if (argc >= 3 && XR_IS_STRING(args[2])) {
        size_t ct_len;
        content_type = xrs_string_arg(args[2], &ct_len);
    }

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)

    XrValue ret;

    // Synchronous call
    XrHttpResult result = xr_http_post(X, url_copy, body, body_len, content_type);
    ret = result_to_json(X, &result);
    xr_http_result_free(&result);

    URL_COPY_END();
    return ret;
}

// http.put(url: string, body?: string, contentType?: string) -> Response
static XrValue http_put(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        fprintf(stderr, "http.put() requires at least 1 argument\n");
        return xr_null();
    }

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url) {
        fprintf(stderr, "http.put() first argument must be a string\n");
        return xr_null();
    }

    const char *body = NULL;
    size_t body_len = 0;
    if (argc >= 2 && XR_IS_STRING(args[1])) {
        body = xrs_string_arg(args[1], &body_len);
    }

    const char *content_type = "application/json";
    if (argc >= 3 && XR_IS_STRING(args[2])) {
        size_t ct_len;
        content_type = xrs_string_arg(args[2], &ct_len);
    }

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)

    XrHttpResult result = xr_http_put(X, url_copy, body, body_len, content_type);
    XrValue ret = result_to_json(X, &result);
    xr_http_result_free(&result);

    URL_COPY_END();
    return ret;
}

// http.delete(url: string) -> Response
static XrValue http_delete(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        fprintf(stderr, "http.delete() requires 1 argument\n");
        return xr_null();
    }

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url) {
        fprintf(stderr, "http.delete() argument must be a string\n");
        return xr_null();
    }

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)

    XrHttpResult result = xr_http_delete(X, url_copy);
    XrValue ret = result_to_json(X, &result);
    xr_http_result_free(&result);

    URL_COPY_END();
    return ret;
}

// http.request(options: Json) -> Response
// options: url (required), method, body, headers, timeout (ms)
static XrValue http_request(XrayIsolate *X, XrValue *args, int argc) {
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

    // Get method
    size_t method_len;
    const char *method = get_json_string(X, options, "method", &method_len);
    if (method && method_len > 0) {
        config.method = xr_http_method_from_string(method, method_len);
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
                    return xr_null();
                }

                int idx = 0;
                uint32_t map_size = xr_map_sizenode(headers_map);
                for (uint32_t i = 0; i < map_size && idx < custom_header_count; i++) {
                    XrMapNode *node = &headers_map->node[i];
                    if (!XR_MAP_NODE_EMPTY(node) && XR_IS_STRING(node->key) &&
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
        XrShape *shape = xr_json_shape(X, headers_json);

        if (shape && shape->field_count > 0) {
            // Fast mode: iterate Shape fields
            XrSymbolTable *symtab = (XrSymbolTable *) X->symbol_table;
            custom_header_count = shape->field_count;
            custom_headers = (XrHttpHeader *) xr_malloc(sizeof(XrHttpHeader) * custom_header_count);
            if (!custom_headers) {
                URL_COPY_END();
                return xr_null();
            }

            int idx = 0;
            for (uint16_t i = 0; i < shape->field_count; i++) {
                SymbolId sym = shape->field_symbols[i];
                XrValue val = xr_json_get_field_any(X, headers_json, i);

                // Get field name from Symbol table
                const char *field_name = xr_symbol_get_name_in_table(symtab, sym);
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

    // Check stream option
    XrValue stream_val = xr_json_get_by_key(X, options, "stream");
    if (XR_IS_BOOL(stream_val) && XR_TO_BOOL(stream_val)) {
        config.stream = true;
    }

    XrValue ret;

    // Synchronous call
    XrHttpResult result = xr_http_request(X, &config);

    if (config.stream && result._stream_conn) {
        // Store result in streams array, return handle
        XrHttpContext *ctx = xr_http_get_context(X);
        int slot = -1;
        if (ctx) {
            for (int i = 0; i < XR_HTTP_MAX_STREAMS; i++) {
                if (!ctx->streams[i]) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot < 0) {
            xr_http_result_free(&result);
            URL_COPY_END();
            if (custom_headers)
                xr_free(custom_headers);
            return xr_null();
        }
        XrHttpResult *sr = (XrHttpResult *) xr_malloc(sizeof(XrHttpResult));
        if (!sr) {
            xr_http_result_free(&result);
            URL_COPY_END();
            if (custom_headers)
                xr_free(custom_headers);
            return xr_null();
        }
        *sr = result;
        ctx->streams[slot] = sr;

        XrJson *json = xr_json_new(xr_current_coro(X), 8);
        xr_json_set_by_key(X, json, "status", xr_int(sr->status_code));
        if (sr->status_text)
            xr_json_set_by_key(X, json, "statusText", xrs_string_value_c(X, sr->status_text));
        XrJson *hdrs = xr_json_new(xr_current_coro(X), 16);
        for (int i = 0; i < sr->header_count; i++) {
            char buf[128];
            size_t kl = sr->headers[i].name_len;
            if (kl >= sizeof(buf))
                kl = sizeof(buf) - 1;
            memcpy(buf, sr->headers[i].name, kl);
            buf[kl] = '\0';
            xr_json_set_by_key(
                X, hdrs, buf,
                xrs_string_value_n(X, sr->headers[i].value, sr->headers[i].value_len));
        }
        xr_json_set_by_key(X, json, "headers", xr_json_value(hdrs));
        xr_json_set_by_key(X, json, "streaming", xr_bool(true));
        xr_json_set_by_key(X, json, "_streamId", xr_int(slot));
        xr_json_set_by_key(X, json, "ok", xr_bool(sr->status_code >= 200 && sr->status_code < 300));
        ret = xr_json_value(json);
    } else {
        ret = result_to_json(X, &result);
        xr_http_result_free(&result);
    }

    // Cleanup
    URL_COPY_END();
    if (custom_headers)
        xr_free(custom_headers);

    return ret;
}

// http.readChunk(resp, maxBytes?) -> string | null
// Read next chunk from streaming response. Returns null on EOF.
static XrValue http_read_chunk(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !xr_value_is_json(args[0]))
        return xr_null();
    XrJson *resp = xr_value_to_json(args[0]);
    XrValue sid_val = xr_json_get_by_key(X, resp, "_streamId");
    if (!XR_IS_INT(sid_val))
        return xr_null();
    int slot = (int) XR_TO_INT(sid_val);

    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx || slot < 0 || slot >= XR_HTTP_MAX_STREAMS || !ctx->streams[slot])
        return xr_null();

    int max_bytes = 8192;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        int v = (int) XR_TO_INT(args[1]);
        if (v > 0 && v <= 1048576)
            max_bytes = v;
    }

    char *buf = (char *) xr_malloc(max_bytes);
    if (!buf)
        return xr_null();

    int n = xr_http_stream_read(ctx->streams[slot], buf, max_bytes);
    if (n <= 0) {
        xr_free(buf);
        return xr_null();
    }
    XrValue result = xrs_string_value_n(X, buf, n);
    xr_free(buf);
    return result;
}

// http.closeStream(resp) -> void
// Close a streaming response and release resources.
static XrValue http_close_stream(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !xr_value_is_json(args[0]))
        return xr_null();
    XrJson *resp = xr_value_to_json(args[0]);
    XrValue sid_val = xr_json_get_by_key(X, resp, "_streamId");
    if (!XR_IS_INT(sid_val))
        return xr_null();
    int slot = (int) XR_TO_INT(sid_val);

    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx || slot < 0 || slot >= XR_HTTP_MAX_STREAMS || !ctx->streams[slot])
        return xr_null();

    xr_http_stream_close(ctx->streams[slot]);
    xr_http_result_free(ctx->streams[slot]);
    xr_free(ctx->streams[slot]);
    ctx->streams[slot] = NULL;
    return xr_null();
}

// http.urlEncode(str: string) -> string
static XrValue http_url_encode(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }

    XrString *input = XR_TO_STRING(args[0]);
    const char *src = input->data;
    size_t src_len = input->length;

    // Estimate output size (worst case: each char becomes %XX)
    size_t out_cap = src_len * 3 + 1;
    char *out = (char *) xr_malloc(out_cap);
    if (!out)
        return xr_null();
    char *p = out;

    static const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = c;
        } else {
            *p++ = '%';
            *p++ = hex[c >> 4];
            *p++ = hex[c & 0x0F];
        }
    }

    XrValue result = xrs_string_value_n(X, out, p - out);
    xr_free(out);
    return result;
}

// http.urlDecode(str: string) -> string
static XrValue http_url_decode(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }

    XrString *input = XR_TO_STRING(args[0]);
    const char *src = input->data;
    size_t src_len = input->length;

    char *out = (char *) xr_malloc(src_len + 1);
    if (!out)
        return xr_null();
    char *p = out;

    for (size_t i = 0; i < src_len; i++) {
        if (src[i] == '%' && i + 2 < src_len) {
            // Parse %XX
            char h = src[i + 1];
            char l = src[i + 2];
            int hv = (h >= '0' && h <= '9')   ? h - '0'
                     : (h >= 'A' && h <= 'F') ? h - 'A' + 10
                     : (h >= 'a' && h <= 'f') ? h - 'a' + 10
                                              : -1;
            int lv = (l >= '0' && l <= '9')   ? l - '0'
                     : (l >= 'A' && l <= 'F') ? l - 'A' + 10
                     : (l >= 'a' && l <= 'f') ? l - 'a' + 10
                                              : -1;
            if (hv >= 0 && lv >= 0) {
                *p++ = (char) ((hv << 4) | lv);
                i += 2;
                continue;
            }
        } else if (src[i] == '+') {
            *p++ = ' ';
            continue;
        }
        *p++ = src[i];
    }

    XrValue result = xrs_string_value_n(X, out, p - out);
    xr_free(out);
    return result;
}

/* ========== HTTP Server Implementation ========== */

/* ========== HTTP Context Management ========== */

// Get HTTP context (stored in module's native_handle)
XrHttpContext *xr_http_get_context(XrayIsolate *X) {
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
        ctx->cookie_jar_enabled = true;  // Cookie enabled by default

        // Initialize per-isolate server config defaults
        atomic_init(&ctx->max_conns, HTTP_DEFAULT_MAX_CONNS);
        atomic_init(&ctx->max_requests_per_conn, 0);
        atomic_init(&ctx->idle_timeout_ms, HTTP_DEFAULT_IDLE_TIMEOUT_MS);
        atomic_init(&ctx->read_timeout_ms, HTTP_DEFAULT_READ_TIMEOUT_MS);
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

    // Free form data
    if (ctx->form_data) {
        xr_form_data_free(ctx->form_data);
        ctx->form_data = NULL;
    }

    // Free Cookie Jar
    if (ctx->cookie_jar) {
        xr_cookie_jar_free(ctx->cookie_jar);
        ctx->cookie_jar = NULL;
    }

    // Free proxy config
    if (ctx->proxy) {
        xr_proxy_config_free(ctx->proxy);
        xr_free(ctx->proxy);
        ctx->proxy = NULL;
    }

    // Free no_proxy list
    for (int i = 0; i < ctx->no_proxy_count; i++) {
        xr_free(ctx->no_proxy[i]);
    }
    xr_free(ctx->no_proxy);
    ctx->no_proxy = NULL;
    ctx->no_proxy_count = 0;

    // Free HTTP/2 server
    if (ctx->h2_server) {
        xr_h2_server_free(ctx->h2_server);
        ctx->h2_server = NULL;
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

    // Close any active streaming responses
    for (int i = 0; i < XR_HTTP_MAX_STREAMS; i++) {
        if (ctx->streams[i]) {
            xr_http_stream_close(ctx->streams[i]);
            xr_http_result_free(ctx->streams[i]);
            xr_free(ctx->streams[i]);
            ctx->streams[i] = NULL;
        }
    }

    xr_free(ctx);
}

// http.route(method, path, handler) - Register route
// handler: function or static string
static XrValue http_route(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 3 || !ctx) {
        return xr_null();
    }

    // Auto-create global server instance
    if (!ctx->server) {
        ctx->server = xr_http_server_new(X);
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

    // Copy path (needs persistence, will be owned by router)
    char *path_copy = (char *) xr_malloc(path_len + 1);
    if (!path_copy)
        return xr_null();
    memcpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';

    // Check third argument type
    XrValue handler_arg = args[2];

    if (XR_IS_CLOSURE(handler_arg)) {
        // Closure callback - register dynamic route
        XrClosure *closure = (XrClosure *) XR_TO_PTR(handler_arg);
        xr_http_server_route(ctx->server, method, path_copy, closure);
    } else if (XR_IS_STRING(handler_arg)) {
        // Static string response - copy and register
        size_t response_len;
        const char *response = xrs_string_arg(handler_arg, &response_len);
        if (response && response_len > 0) {
            // Copy response (will be owned by router)
            char *response_copy = (char *) xr_malloc(response_len + 1);
            if (!response_copy) {
                xr_free(path_copy);
                return xr_null();
            }
            memcpy(response_copy, response, response_len);
            response_copy[response_len] = '\0';

            xr_http_server_static(ctx->server, method, path_copy, response_copy, response_len);
        } else {
            xr_free(path_copy);
        }
    } else if (xr_value_is_json(handler_arg)) {
        // Json object - serialize in C layer and register as static prebuilt
        size_t json_len = 0;
        char *json_str = xr_json_stringify_to_cstr(X, handler_arg, &json_len);
        if (json_str && json_len > 0) {
            xr_http_server_static(ctx->server, method, path_copy, json_str, json_len);
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

// http.static(method, path, content) - Register static route (pre-built response)
static XrValue http_static(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 3 || !ctx) {
        return xr_null();
    }

    // Auto-create global server instance
    if (!ctx->server) {
        ctx->server = xr_http_server_new(X);
        if (!ctx->server) {
            fprintf(stderr, "http.static: failed to create server\n");
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

    // Copy path (needs persistence)
    char *path_copy = (char *) xr_malloc(path_len + 1);
    if (!path_copy)
        return xr_null();
    memcpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';

    // Get response content
    size_t response_len;
    const char *response = xrs_string_arg(args[2], &response_len);
    if (!response) {
        xr_free(path_copy);
        return xr_null();
    }

    // Copy response content
    char *response_copy = (char *) xr_malloc(response_len + 1);
    if (!response_copy) {
        xr_free(path_copy);
        return xr_null();
    }
    memcpy(response_copy, response, response_len);
    response_copy[response_len] = '\0';

    // Register static route (pre-built response)
    xr_router_add_static(ctx->server->router, method, path_copy, response_copy, response_len);

    return xr_null();
}

// http.ws(path, handler) - Register WebSocket upgrade route on HTTP server
// When a GET request with Upgrade:websocket hits this path, the connection
// is upgraded in-place and handler(wsConn) is called.
static XrValue http_ws_route(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 2 || !ctx)
        return xr_null();

    if (!ctx->server) {
        ctx->server = xr_http_server_new(X);
        if (!ctx->server)
            return xr_null();
    }

    size_t path_len;
    const char *path = xrs_string_arg(args[0], &path_len);
    if (!path)
        return xr_null();

    if (!XR_IS_CLOSURE(args[1]))
        return xr_null();
    XrClosure *closure = (XrClosure *) XR_TO_PTR(args[1]);

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

// http.setConnHandler(handler) - Set connection handler closure
// Handler receives client_fd, handles entire connection lifecycle
static XrValue http_set_conn_handler(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 1 || !ctx) {
        return xr_null();
    }

    // Auto-create global server instance
    if (!ctx->server) {
        ctx->server = xr_http_server_new(X);
        if (!ctx->server) {
            fprintf(stderr, "http.setConnHandler: failed to create server\n");
            return xr_null();
        }
    }

    if (XR_IS_CLOSURE(args[0])) {
        XrClosure *closure = (XrClosure *) XR_TO_PTR(args[0]);

        /*
         * Check if closure has upvalues.
         * HTTP callback closures should not have mutable upvalues because:
         *   1. Closures are not deep-copied (performance optimization)
         *   2. Shared upvalues may cause data races
         * Use global variables or parameter passing for shared state.
         */
        if (closure->upval_count > 0) {
            fprintf(stderr, "Warning: http.setConnHandler closure captures external variables\n");
            fprintf(stderr, "         HTTP callback should not capture external variables\n");
            // Still allow setting, but warn
        }

        ctx->server->conn_handler_closure = closure;
    }

    return xr_null();
}

// http.__getConnHandler() -> closure | null (for http.xr)
static XrValue http_get_conn_handler(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx || !ctx->server || !ctx->server->conn_handler_closure) {
        return xr_null();
    }
    return XR_FROM_PTR(ctx->server->conn_handler_closure);
}

/* ========== http.listen migrated to http.xr ========== */

/*
 * C layer listen removed, using high-performance http.xr script implementation
 * Performance: C layer ~1.6K QPS vs script layer ~130K QPS
 */

// http.stopServer() -> void
static XrValue http_stop_server(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrHttpContext *ctx = xr_http_get_context(X);
    if (ctx && ctx->server) {
        xr_http_server_stop(ctx->server);
    }

    return xr_null();
}

/* ========== Modular High-Performance HTTP API ========== */

// http.parseRequest(fd) -> [method, path, keepAlive] or null
// High-performance parsing, returns array to avoid JSON overhead
static XrValue http_parse_request_fast(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    int fd = XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : -1;
    if (fd < 0)
        return xr_null();

    // Read request data
    char buf[4096];
    int n = xr_socket_read(X, fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return xr_null();
    buf[n] = '\0';

    // Parse request line
    char *line_end = strstr(buf, "\r\n");
    if (!line_end)
        return xr_null();
    *line_end = '\0';

    // Parse "GET /path HTTP/1.1"
    char *method_end = strchr(buf, ' ');
    if (!method_end)
        return xr_null();
    *method_end = '\0';

    char *path_start = method_end + 1;
    char *path_end = strchr(path_start, ' ');
    if (!path_end)
        return xr_null();
    *path_end = '\0';

    // Check keep-alive
    bool keep_alive = true;
    char *conn = strstr(line_end + 2, "Connection:");
    if (conn) {
        if (strstr(conn, "close"))
            keep_alive = false;
    }

    // Return array [method, path, keepAlive]
    XrArray *arr = xr_array_new(xr_current_coro(X));
    xr_array_push(arr, xr_string_value(xr_string_new(X, buf, method_end - buf)));
    xr_array_push(arr, xr_string_value(xr_string_new(X, path_start, path_end - path_start)));
    xr_array_push(arr, xr_bool(keep_alive));

    return XR_FROM_PTR(arr);
}

// http.sendResponse(fd, body, status?) -> bool (high-performance)
static XrValue http_send_response_fast(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2)
        return xr_bool(false);

    int fd = XR_IS_INT(args[0]) ? (int) XR_TO_INT(args[0]) : -1;
    if (fd < 0)
        return xr_bool(false);

    size_t body_len;
    const char *body = xrs_string_arg(args[1], &body_len);
    if (!body) {
        body = "";
        body_len = 0;
    }

    int status = (argc >= 3 && XR_IS_INT(args[2])) ? (int) XR_TO_INT(args[2]) : 200;

    // Build response
    char header[256];
    const char *status_text = (status == 200)   ? "OK"
                              : (status == 404) ? "Not Found"
                              : (status == 500) ? "Internal Server Error"
                                                : "OK";

    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: text/plain\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: keep-alive\r\n"
                              "\r\n",
                              status, status_text, body_len);

    // Send header + body
    int n1 = xr_socket_write(X, fd, header, header_len);
    if (n1 <= 0)
        return xr_bool(false);

    if (body_len > 0) {
        int n2 = xr_socket_write(X, fd, body, body_len);
        if (n2 <= 0)
            return xr_bool(false);
    }

    return xr_bool(true);
}

/* ========== Streaming Download API ========== */

// http.download(url, path) -> Json (streaming download to local file)
static XrValue http_download(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2)
        return xr_null();

    size_t url_len, path_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    const char *path = xrs_string_arg(args[1], &path_len);
    if (!url || !path)
        return xr_null();

    // URL/Path copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)
    char _path_stack_buf[URL_STACK_SIZE];
    char *path_copy;
    bool _path_need_free = false;
    if (path_len < URL_STACK_SIZE) {
        path_copy = _path_stack_buf;
    } else {
        path_copy = (char *) xr_malloc(path_len + 1);
        _path_need_free = true;
    }
    memcpy(path_copy, path, path_len);
    path_copy[path_len] = '\0';

    // Execute download
    XrStreamResult result = xr_http_download(url_copy, path_copy, NULL, NULL);

    URL_COPY_END();
    if (_path_need_free)
        xr_free(path_copy);

    // Build return result
    XrJson *json = xr_json_new(xr_current_coro(X), 8);
    xr_json_set_by_key(X, json, "status", xr_int(result.status_code));
    xr_json_set_by_key(X, json, "downloaded", xr_int((int64_t) result.downloaded));
    xr_json_set_by_key(X, json, "total", xr_int((int64_t) result.total_size));
    xr_json_set_by_key(X, json, "completed", xr_bool(result.completed));
    if (result.error_msg) {
        xr_json_set_by_key(X, json, "error", xrs_string_value_c(X, result.error_msg));
    }

    xr_stream_result_free(&result);

    return xr_json_value(json);
}

// http.getContentLength(url) -> int (HEAD request for file size)
static XrValue http_get_content_length(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_int(-1);

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url)
        return xr_int(-1);

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)
    long long size = xr_http_get_content_length(X, url_copy);
    URL_COPY_END();

    return xr_int(size);
}

/* ========== Multipart Form API ========== */

// http.formDataNew(maxTotalSize?, maxFileSize?) -> bool
// Pass 0 to disable the corresponding limit.
// Omit to use defaults (64MB total, 32MB per file).
static XrValue http_form_data_new(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx)
        return xr_bool(false);

    if (ctx->form_data) {
        xr_form_data_free(ctx->form_data);
    }
    ctx->form_data = xr_form_data_new();
    if (!ctx->form_data)
        return xr_bool(false);

    // Optional: override max total size
    if (argc >= 1 && XR_IS_INT(args[0])) {
        int64_t v = XR_TO_INT(args[0]);
        ctx->form_data->max_total_size = (v <= 0) ? 0 : (size_t) v;
    }
    // Optional: override max per-file size
    if (argc >= 2 && XR_IS_INT(args[1])) {
        int64_t v = XR_TO_INT(args[1]);
        ctx->form_data->max_file_size = (v <= 0) ? 0 : (size_t) v;
    }

    return xr_bool(true);
}

// http.formDataAppend(name, value) -> void
static XrValue http_form_data_append(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 2 || !ctx || !ctx->form_data)
        return xr_null();

    size_t name_len, value_len;
    const char *name = xrs_string_arg(args[0], &name_len);
    const char *value = xrs_string_arg(args[1], &value_len);
    if (!name || !value)
        return xr_null();

    // Need null-terminated string
    char *name_copy = (char *) xr_malloc(name_len + 1);
    if (!name_copy)
        return xr_null();
    memcpy(name_copy, name, name_len);
    name_copy[name_len] = '\0';

    xr_form_data_append(ctx->form_data, name_copy, value, value_len);
    xr_free(name_copy);

    return xr_null();
}

// http.formDataAppendFile(name, filepath) -> bool
static XrValue http_form_data_append_file(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 2 || !ctx || !ctx->form_data)
        return xr_bool(false);

    size_t name_len, path_len;
    const char *name = xrs_string_arg(args[0], &name_len);
    const char *path = xrs_string_arg(args[1], &path_len);
    if (!name || !path)
        return xr_bool(false);

    char *name_copy = (char *) xr_malloc(name_len + 1);
    if (!name_copy)
        return xr_bool(false);
    char *path_copy = (char *) xr_malloc(path_len + 1);
    if (!path_copy) {
        xr_free(name_copy);
        return xr_bool(false);
    }
    memcpy(name_copy, name, name_len);
    memcpy(path_copy, path, path_len);
    name_copy[name_len] = '\0';
    path_copy[path_len] = '\0';

    int ret = xr_form_data_append_file_path(ctx->form_data, name_copy, path_copy);

    xr_free(name_copy);
    xr_free(path_copy);

    return xr_bool(ret == 0);
}

// http.formDataPost(url) -> Json
static XrValue http_form_data_post(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (argc < 1 || !ctx || !ctx->form_data)
        return xr_null();

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url)
        return xr_null();

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)

    // Build form data
    char *body = NULL;
    size_t body_len = 0;
    char *content_type = NULL;

    if (xr_form_data_build(ctx->form_data, &body, &body_len, &content_type) < 0) {
        URL_COPY_END();
        return xr_null();
    }

    // Send POST request
    XrHttpResult result = xr_http_post(X, url_copy, body, body_len, content_type);

    URL_COPY_END();
    xr_free(body);
    xr_free(content_type);

    // Clean up form data
    xr_form_data_free(ctx->form_data);
    ctx->form_data = NULL;

    // Build return result
    XrJson *json = xr_json_new(xr_current_coro(X), 4);
    xr_json_set_by_key(X, json, "status", xr_int(result.status_code));
    if (result.body && result.body_len > 0) {
        xr_json_set_by_key(X, json, "body", xrs_string_value_n(X, result.body, result.body_len));
    }
    xr_http_result_free(&result);

    return xr_json_value(json);
}

/* ========== Proxy Settings API ========== */

// http.setProxy(url) -> void (format: http://[user:pass@]host:port)
static XrValue http_set_proxy(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1)
        return xr_null();

    size_t url_len;
    const char *url = xrs_string_arg(args[0], &url_len);
    if (!url)
        return xr_null();

    // URL copy (stack allocation optimization)
    URL_COPY_BEGIN(url, url_len)
    xr_set_proxy(X, url_copy);
    URL_COPY_END();

    return xr_null();
}

// http.clearProxy() -> void
static XrValue http_clear_proxy(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    xr_clear_proxy(X);
    return xr_null();
}

/* ========== Type Declarations (parsed by gen_stdlib_types.py) ========== */

#include "../../src/module/xbuiltin_decl.h"

// @module http
// @handle HttpResponse { const status: int, const statusText: string, const headers: Json, const
// body: string, const error: string, const ok: bool }
// @handle HttpRequest { const method: string, const path: string, const query: Json, const headers:
// Json, const contentLength: int, const bodyOffset: int }
// @handle DownloadResult { const status: int, const downloaded: int, const total: int, const
// completed: bool, const error: string }

XR_DEFINE_BUILTIN(http_get, "get", "(url: string, options?: Json): HttpResponse",
                  "HTTP GET request")
XR_DEFINE_BUILTIN(http_post, "post", "(url: string, body?: string, options?: Json): HttpResponse",
                  "HTTP POST request")
XR_DEFINE_BUILTIN(http_put, "put", "(url: string, body?: string, options?: Json): HttpResponse",
                  "HTTP PUT request")
XR_DEFINE_BUILTIN(http_delete, "delete", "(url: string, options?: Json): HttpResponse",
                  "HTTP DELETE request")
XR_DEFINE_BUILTIN(http_request, "request",
                  "(method: string, url: string, options?: Json): HttpResponse",
                  "Generic HTTP request")
XR_DEFINE_BUILTIN(http_url_encode, "urlEncode", "(s: string): string", "URL-encode a string")
XR_DEFINE_BUILTIN(http_url_decode, "urlDecode", "(s: string): string", "URL-decode a string")
XR_DEFINE_BUILTIN(http_route, "route", "(method: string, path: string, handler: fn): void",
                  "Register a route handler")
XR_DEFINE_BUILTIN(http_static, "static", "(prefix: string, dir: string): void",
                  "Serve static files from directory")
XR_DEFINE_BUILTIN(http_stop_server, "stopServer", "(): void", "Stop the HTTP server")
XR_DEFINE_BUILTIN(http_parse_request_fast, "parseRequest", "(data: string): HttpRequest?",
                  "Parse raw HTTP request data")
XR_DEFINE_BUILTIN(http_send_response_fast, "sendResponse",
                  "(fd: int, status: int, headers: Json, body: string): int",
                  "Send HTTP response on fd")
XR_DEFINE_BUILTIN(http_download, "download",
                  "(url: string, path: string, options?: Json): DownloadResult",
                  "Download file from URL")
XR_DEFINE_BUILTIN(http_get_content_length, "getContentLength", "(url: string): int",
                  "Get content length of URL")
XR_DEFINE_BUILTIN(http_form_data_new, "formDataNew", "(): Json", "Create new multipart form data")
XR_DEFINE_BUILTIN(http_form_data_append, "formDataAppend",
                  "(form: Json, name: string, value: string): void", "Append field to form data")
XR_DEFINE_BUILTIN(http_form_data_append_file, "formDataAppendFile",
                  "(form: Json, name: string, path: string, filename?: string): void",
                  "Append file to form data")
XR_DEFINE_BUILTIN(http_form_data_post, "formDataPost",
                  "(url: string, form: Json, options?: Json): HttpResponse",
                  "POST multipart form data")
XR_DEFINE_BUILTIN(http_set_proxy, "setProxy", "(url: string): void", "Set HTTP proxy")
XR_DEFINE_BUILTIN(http_clear_proxy, "clearProxy", "(): void", "Clear HTTP proxy")
XR_DEFINE_BUILTIN(h2_get, "h2Get", "(url: string, options?: Json): HttpResponse",
                  "HTTP/2 GET request")
XR_DEFINE_BUILTIN(h2_post, "h2Post", "(url: string, body?: string, options?: Json): HttpResponse",
                  "HTTP/2 POST request")
XR_DEFINE_BUILTIN(h2_request, "h2Request",
                  "(method: string, url: string, options?: Json): HttpResponse",
                  "Generic HTTP/2 request")

/* ========== Module Loading ========== */

XR_FUNC XrModule *xr_load_module_http(XrayIsolate *isolate) {
    // 1. Create Native module
    XrModule *mod = xr_module_create_native(isolate, "http");
    if (!mod)
        return NULL;

    // HTTP client methods (blocking I/O, mark SLOW for immediate P/M handoff)
    XRS_EXPORT_SLOW(mod, isolate, "get", http_get);
    XRS_EXPORT_SLOW(mod, isolate, "post", http_post);
    XRS_EXPORT_SLOW(mod, isolate, "put", http_put);
    XRS_EXPORT_SLOW(mod, isolate, "delete", http_delete);
    XRS_EXPORT_SLOW(mod, isolate, "request", http_request);

    // Utility functions
    XRS_EXPORT(mod, isolate, "urlEncode", http_url_encode);
    XRS_EXPORT(mod, isolate, "urlDecode", http_url_decode);

    // Server functions (per-connection coroutine, singleton mode)
    XRS_EXPORT(mod, isolate, "route", http_route);
    XRS_EXPORT(mod, isolate, "static", http_static);
    XRS_EXPORT(mod, isolate, "setConnHandler", http_set_conn_handler);
    XRS_EXPORT(mod, isolate, "__getConnHandler", http_get_conn_handler);
    XRS_EXPORT_YIELDABLE(mod, isolate, "listen", xr_http_listen_impl);
    XRS_EXPORT(mod, isolate, "config", xr_http_config_impl);
    XRS_EXPORT(mod, isolate, "response", xr_http_response_impl);
    XRS_EXPORT(mod, isolate, "serverStats", xr_http_server_stats);
    XRS_EXPORT(mod, isolate, "ws", http_ws_route);
    XRS_EXPORT_SLOW(mod, isolate, "readChunk", http_read_chunk);
    XRS_EXPORT(mod, isolate, "closeStream", http_close_stream);
    XRS_EXPORT(mod, isolate, "stopServer", http_stop_server);

    // Modular high-performance API (public)
    XRS_EXPORT(mod, isolate, "parseRequest", http_parse_request_fast);
    XRS_EXPORT(mod, isolate, "sendResponse", http_send_response_fast);

    // Streaming download (blocking I/O)
    XRS_EXPORT_SLOW(mod, isolate, "download", http_download);
    XRS_EXPORT(mod, isolate, "getContentLength", http_get_content_length);

    // Multipart form
    XRS_EXPORT(mod, isolate, "formDataNew", http_form_data_new);
    XRS_EXPORT(mod, isolate, "formDataAppend", http_form_data_append);
    XRS_EXPORT(mod, isolate, "formDataAppendFile", http_form_data_append_file);
    XRS_EXPORT(mod, isolate, "formDataPost", http_form_data_post);

    // Proxy settings
    XRS_EXPORT(mod, isolate, "setProxy", http_set_proxy);
    XRS_EXPORT(mod, isolate, "clearProxy", http_clear_proxy);

    // NOTE: WebSocket functions moved to separate 'ws' module

    // HTTP/2 client functions
    extern XrValue h2_get(XrayIsolate * X, XrValue * args, int argc);
    extern XrValue h2_post(XrayIsolate * X, XrValue * args, int argc);
    extern XrValue h2_request(XrayIsolate * X, XrValue * args, int argc);
    XRS_EXPORT(mod, isolate, "h2Get", h2_get);
    XRS_EXPORT(mod, isolate, "h2Post", h2_post);
    XRS_EXPORT(mod, isolate, "h2Request", h2_request);

    // HTTP/2 server functions
    extern XrValue h2_create_server(XrayIsolate * X, XrValue * args, int argc);
    extern XrValue h2_server_listen(XrayIsolate * X, XrValue * args, int argc);
    extern XrValue h2_server_stop(XrayIsolate * X, XrValue * args, int argc);
    extern XrValue h2_push(XrayIsolate * X, XrValue * args, int argc);
    XRS_EXPORT(mod, isolate, "h2CreateServer", h2_create_server);
    XRS_EXPORT(mod, isolate, "h2Listen", h2_server_listen);
    XRS_EXPORT(mod, isolate, "h2Stop", h2_server_stop);
    XRS_EXPORT(mod, isolate, "h2Push", h2_push);

    // 3. Mark as loaded
    mod->loaded = true;
    return mod;
}
