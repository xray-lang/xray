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
 *   Uses Json type uniformly for input/output
 */

#include "http_internal.h"
#include "../../src/base/xmalloc.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xpanic_info.h"
#include "../net/tls.h"
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

// Helper function: create string value
static XrValue make_str(XrVMRuntime *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xr_null();
    XrString *str = xr_string_intern(X, s, len, 0);
    return xr_string_value(str);
}

static XrValue make_cstr(XrVMRuntime *X, const char *s) {
    if (!s)
        return xr_null();
    return make_str(X, s, strlen(s));
}

// Get string field from Json
static const char *json_get_string(XrVMRuntime *X, XrJson *json, const char *key, size_t *len) {
    XrValue val = xr_json_get_by_key(X, json, key);
    if (!XR_IS_STRING(val)) {
        if (len)
            *len = 0;
        return NULL;
    }
    XrString *s = XR_TO_STRING(val);
    if (len)
        *len = s->length;
    return s->data;
}

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
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
           c == '*' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
           c == '`' || c == '|' || c == '~';
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
        ascii_ieq(h->name, h->name_len, "upgrade") ||
        ascii_ieq(h->name, h->name_len, "host"))
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

/* ========== Header Extraction (Json/Map -> XrHttpHeader[]) ========== */

/*
 * Parse the `headers` field of an HTTP options Json into an
 * XrHttpHeader array. Caller owns the returned array and must
 * xr_free() it after use. Supports both Map<string,string> and Json
 * object shapes — mirrors what http.c does for HTTP/1.1 requests.
 *
 * Returns NULL on allocation failure or when no headers are present.
 * On success, *out_count holds the number of headers extracted.
 */
static XrHttpHeader *extract_headers_from_options(XrVMRuntime *X, XrJson *opts, int *out_count) {
    *out_count = 0;
    XrValue headers_val = xr_json_get_by_key(X, opts, "headers");

    if (xr_value_is_map(headers_val)) {
        XrMap *m = xr_value_to_map(headers_val);
        if (!m || m->count == 0)
            return NULL;
        XrHttpHeader *out = (XrHttpHeader *) xr_malloc(sizeof(XrHttpHeader) * m->count);
        if (!out)
            return NULL;
        uint32_t idx = 0;
        uint32_t map_size = m->nentries;
        for (uint32_t i = 0; i < map_size && idx < m->count; i++) {
            XrMapEntry *node = &m->entries[i];
            if (!XR_MAP_ENTRY_EMPTY(node) && XR_IS_STRING(node->key) && XR_IS_STRING(node->value)) {
                XrString *k = XR_TO_STRING(node->key);
                XrString *v = XR_TO_STRING(node->value);
                out[idx].name = k->data;
                out[idx].name_len = k->length;
                out[idx].value = v->data;
                out[idx].value_len = v->length;
                idx++;
            }
        }
        *out_count = (int) idx;
        return out;
    }

    if (xr_value_is_json(headers_val)) {
        XrJson *hjson = xr_value_to_json(headers_val);
        XrClass *cls = hjson->klass;
        if (!cls || cls->field_count == 0)
            return NULL;
        XrHttpHeader *out = (XrHttpHeader *) xr_malloc(sizeof(XrHttpHeader) * cls->field_count);
        if (!out)
            return NULL;
        int idx = 0;
        for (uint16_t i = 0; i < cls->field_count; i++) {
            XrValue val = xr_instance_get_dynamic_field(hjson, i);
            const char *field_name = cls->fields[i].name;
            if (field_name && XR_IS_STRING(val)) {
                XrString *v = XR_TO_STRING(val);
                out[idx].name = field_name;
                out[idx].name_len = strlen(field_name);
                out[idx].value = v->data;
                out[idx].value_len = v->length;
                idx++;
            }
        }
        *out_count = idx;
        return out;
    }

    return NULL;
}

/* ========== HTTP/2 Response Building ========== */

// Convert HTTP/2 response to Json
static XrValue h2_response_to_json(XrVMRuntime *X, XrH2Response *resp) {
    XrJson *json = xr_json_new(xr_current_coro(X));

    // status
    xr_json_set_by_key(X, json, "status", xr_int(resp->status));

    // body
    if (resp->body && resp->body_len > 0) {
        xr_json_set_by_key(X, json, "body", make_str(X, resp->body, resp->body_len));
    } else {
        xr_json_set_by_key(X, json, "body", make_cstr(X, ""));
    }

    // ok
    xr_json_set_by_key(X, json, "ok", xr_bool(resp->status >= 200 && resp->status < 300));

    return xr_json_value(json);
}

/* ========== HTTP/2 Client Binding ========== */

/*
 * http.h2Request(options: Json) -> Json
 *
 * options: {
 *   url: string,
 *   method?: string,
 *   body?: string,
 *   headers?: Json
 * }
 */
XrValue h2_request(XrVMRuntime *X, XrValue *args, int argc) {
    if (argc < 1 || !xr_value_is_json(args[0])) {
        return xr_null();
    }

    XrHttpContext *ctx = http_get_context(X);
    if (!ctx)
        return xr_null();
    if (!ctx->h2_client_pool) {
        ctx->h2_client_pool = http2_client_pool_create();
        if (!ctx->h2_client_pool)
            return xr_null();
    }

    XrJson *opts = xr_value_to_json(args[0]);

    const char *url = json_get_string(X, opts, "url", NULL);
    if (!url)
        return xr_null();

    XrH2Request req = {0};
    req.method = json_get_string(X, opts, "method", &req.method_len);
    req.body = json_get_string(X, opts, "body", &req.body_len);

    int hcount = 0;
    XrHttpHeader *headers = extract_headers_from_options(X, opts, &hcount);
    if (hcount > 0) {
        req.headers = headers;
        req.header_count = hcount;
    }

    if (!h2_request_options_valid(&req)) {
        if (headers)
            xr_free(headers);
        return xr_null();
    }

    XrH2Response *resp = http2_client_request(X, ctx->h2_client_pool, url, &req);

    if (headers)
        xr_free(headers);

    if (!resp) {
        if (!xr_tls_is_available())
            throw_tls_unavailable(X);
        return xr_null();
    }

    XrValue result = h2_response_to_json(X, resp);
    http2_client_response_free(resp);
    return result;
}
