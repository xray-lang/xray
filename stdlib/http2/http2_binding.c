/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http2_binding.c - HTTP/2 xray binding
 *
 * KEY CONCEPT:
 *   Exposes a private typed tuple/byte-array boundary to http.xr. Public
 *   request/response semantics stay in the Xray control plane.
 */

#include "http2_internal.h"
#include "../../stdlib/common.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xpanic_info.h"
#include "../../src/runtime/object/xtuple.h"
#include "../../stdlib/net/tls.h"
#include <string.h>

// clang-format off
static const char TLS_UNAVAIL_MSG[] =
    "HTTPS requires TLS support. Xray was built without OpenSSL"
    " -- rebuild with: cmake -DENABLE_TLS=ON -DOPENSSL_ROOT_DIR=<path>";

static void throw_tls_unavailable(XrVMRuntime *X) {
    XrValue exc = xr_panic_info_new(X, XR_ERR_TLS_UNAVAILABLE, TLS_UNAVAIL_MSG);
    xr_vm_unwind_with_trace(X, exc);
}
// clang-format on

// External declarations
extern XrValue xr_string_value(XrString *str);
extern XrString *xr_string_intern(XrVMRuntime *X, const char *str, size_t len, uint32_t hash);

typedef struct XrHttp2Context {
    XrH2Pool *client_pool;
} XrHttp2Context;

static XrHttp2Context *http2_get_context(XrVMRuntime *X) {
    XrModuleRegistry *registry = X ? (XrModuleRegistry *) X->module_registry : NULL;
    XrModule *module = registry && registry->loaded_modules
                           ? (XrModule *) xr_hashmap_get(registry->loaded_modules, "http2")
                           : NULL;
    if (!module)
        return NULL;
    if (!module->native_handle)
        module->native_handle = xr_calloc(1, sizeof(XrHttp2Context));
    return (XrHttp2Context *) module->native_handle;
}

static void http2_context_destroy(void *handle) {
    XrHttp2Context *ctx = (XrHttp2Context *) handle;
    if (!ctx)
        return;
    if (ctx->client_pool)
        http2_client_pool_destroy(ctx->client_pool);
    xr_free(ctx);
}

// Helper function: create string value
static bool ascii_ieq(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        unsigned char ca = (unsigned char) a[i];
        unsigned char cb = (unsigned char) b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char) (ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char) (cb + ('a' - 'A'));
        if (ca != cb)
            return false;
    }
    return true;
}

static bool h2_token_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '!' ||
           c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
           c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

static bool h2_method_valid(const char *method, size_t len) {
    if (!method)
        return true;
    if (len == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!h2_token_char((unsigned char) method[i]))
            return false;
    }
    return true;
}

static bool h2_header_name_valid(const char *name, size_t len) {
    if (!name || len == 0 || name[0] == ':')
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!h2_token_char((unsigned char) name[i]))
            return false;
    }
    return true;
}

static bool h2_header_value_valid(const char *value, size_t len) {
    if (!value && len > 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) value[i];
        if (c == '\t')
            continue;
        if (c < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

static bool h2_custom_header_allowed(const XrHttpHeader *h) {
    if (!h2_header_name_valid(h->name, h->name_len) ||
        !h2_header_value_valid(h->value, h->value_len))
        return false;

    if (ascii_ieq(h->name, h->name_len, "connection") ||
        ascii_ieq(h->name, h->name_len, "keep-alive") ||
        ascii_ieq(h->name, h->name_len, "proxy-connection") ||
        ascii_ieq(h->name, h->name_len, "transfer-encoding") ||
        ascii_ieq(h->name, h->name_len, "upgrade") || ascii_ieq(h->name, h->name_len, "host"))
        return false;

    if (ascii_ieq(h->name, h->name_len, "te") && !ascii_ieq(h->value, h->value_len, "trailers"))
        return false;

    return true;
}

static bool h2_request_options_valid(const XrH2Request *req) {
    if (!req)
        return true;
    if (!h2_method_valid(req->method, req->method_len))
        return false;
    for (int i = 0; i < req->header_count; i++) {
        if (!h2_custom_header_allowed(&req->headers[i]))
            return false;
    }
    return true;
}

/* ========== Typed Header/Response Conversion ========== */

static bool h2_typed_headers(XrArray *names, XrArray *values, XrHttpHeader **out_headers,
                             int *out_count) {
    *out_headers = NULL;
    *out_count = 0;
    if (!names || !values || names->length < 0 || names->length != values->length ||
        names->length > 28)
        return false;
    if (names->length == 0)
        return true;

    XrHttpHeader *headers =
        (XrHttpHeader *) xr_calloc((size_t) names->length, sizeof(XrHttpHeader));
    if (!headers)
        return false;
    for (int64_t i = 0; i < names->length; i++) {
        XrValue name_value = xr_array_get(names, (int) i);
        XrValue value_value = xr_array_get(values, (int) i);
        if (!XR_IS_STRING(name_value) || !XR_IS_STRING(value_value)) {
            xr_free(headers);
            return false;
        }
        XrString *name = XR_TO_STRING(name_value);
        XrString *value = XR_TO_STRING(value_value);
        headers[i].name = name->data;
        headers[i].name_len = name->length;
        headers[i].value = value->data;
        headers[i].value_len = value->length;
    }
    *out_headers = headers;
    *out_count = (int) names->length;
    return true;
}

static XrValue h2_typed_response(XrVMRuntime *X, const XrH2Response *response) {
    if (!response || response->body_len > INT32_MAX || response->header_count < 0)
        return xr_null();
    XrCoroutine *coro = xr_current_coro(X);
    XrArray *header_names = xr_array_new(coro);
    XrArray *header_values = xr_array_new(coro);
    XrArray *body = xr_byte_array_new(coro, (int32_t) response->body_len);
    if (!header_names || !header_values || !body)
        return xr_null();
    for (int i = 0; i < response->header_count; i++) {
        const XrHttpHeader *header = &response->headers[i];
        XrString *name = xr_string_intern(X, header->name, header->name_len, 0);
        XrString *value = xr_string_intern(X, header->value, header->value_len, 0);
        if (!name || !value)
            return xr_null();
        xr_array_push(header_names, xr_string_value(name));
        xr_array_push(header_values, xr_string_value(value));
    }
    if (response->body_len > 0)
        memcpy(body->data, response->body, response->body_len);
    body->length = (int32_t) response->body_len;

    XrValue fields[4] = {xr_int(response->status), xr_value_from_array(header_names),
                         xr_value_from_array(header_values), xr_value_from_array(body)};
    XrTuple *tuple = xr_tuple_from_values(coro, fields, 4);
    return tuple ? xr_value_from_tuple(tuple) : xr_null();
}

/* ========== HTTP/2 Client Binding ========== */

XrValue h2_supported(XrVMRuntime *X, XrValue *args, int argc) {
    (void) X;
    (void) args;
    (void) argc;
    return xr_bool(xr_tls_is_available());
}

/*
 * http.__h2Request(url, method, headerNames, headerValues, body, timeoutMs)
 *     -> (int, Array<byte>)?
 */
XrValue h2_request_typed(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 6 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]) || !XR_IS_ARRAY(args[2]) ||
        !XR_IS_ARRAY(args[3]) || !XR_IS_ARRAY(args[4]) || !XR_IS_INT(args[5])) {
        return xr_null();
    }

    XrHttp2Context *ctx = http2_get_context(X);
    if (!ctx)
        return xr_null();
    if (!ctx->client_pool) {
        ctx->client_pool = http2_client_pool_create();
        if (!ctx->client_pool)
            return xr_null();
    }

    XrString *url = XR_TO_STRING(args[0]);
    XrString *method = XR_TO_STRING(args[1]);
    XrArray *header_names = XR_TO_ARRAY(args[2]);
    XrArray *header_values = XR_TO_ARRAY(args[3]);
    XrArray *body = XR_TO_ARRAY(args[4]);
    int64_t timeout_value = XR_TO_INT(args[5]);
    if (body->elem_type != XR_ELEM_U8 || body->length < 0 || timeout_value <= 0 ||
        timeout_value > INT32_MAX)
        return xr_null();
    int timeout_ms = (int) timeout_value;

    XrH2Request req = {0};
    req.method = method->data;
    req.method_len = method->length;
    req.body = (const char *) body->data;
    req.body_len = (size_t) body->length;

    int hcount = 0;
    XrHttpHeader *headers = NULL;
    if (!h2_typed_headers(header_names, header_values, &headers, &hcount))
        return xr_null();
    if (hcount > 0) {
        req.headers = headers;
        req.header_count = hcount;
    }

    if (!h2_request_options_valid(&req)) {
        xr_free(headers);
        return xr_null();
    }

    XrH2Response *resp = http2_client_request(X, ctx->client_pool, url->data, &req, timeout_ms);

    xr_free(headers);

    if (!resp) {
        if (!xr_tls_is_available())
            throw_tls_unavailable(X);
        return xr_null();
    }

    XrValue result = h2_typed_response(X, resp);
    http2_client_response_free(resp);
    return result;
}

#define XR_STDLIB_VM_BIND_MODULE_HTTP2 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_HTTP2

XR_FUNC XrModule *xr_load_module_http2(XrVMRuntime *isolate) {
    XrModule *module = xr_module_create_native(isolate, "http2");
    if (!module)
        return NULL;
    module->native_handle_destroy = http2_context_destroy;
    xr_stdlib_vm_bind_http2_generated(isolate, module);
    return module;
}
