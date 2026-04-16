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

#include "http2_client.h"
#include "http2_server.h"
#include "http.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include <string.h>

// External declarations
extern XrValue xr_string_value(XrString *str);
extern XrString* xr_string_intern(XrayIsolate *X, const char *str, size_t len, uint32_t hash);

// Helper function: create string value
static XrValue make_str(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0) return xr_null();
    XrString *str = xr_string_intern(X, s, len, 0);
    return xr_string_value(str);
}

static XrValue make_cstr(XrayIsolate *X, const char *s) {
    if (!s) return xr_null();
    return make_str(X, s, strlen(s));
}

// Helper function: get C string from XrValue
static const char* get_cstring(XrValue val, size_t *len) {
    if (!XR_IS_STRING(val)) return NULL;
    XrString *s = XR_TO_STRING(val);
    if (len) *len = s->length;
    return s->data;
}

// Get string field from Json
static const char* json_get_string(XrayIsolate *X, XrJson *json, const char *key, size_t *len) {
    XrValue val = xr_json_get_by_key(X, json, key);
    if (!XR_IS_STRING(val)) {
        if (len) *len = 0;
        return NULL;
    }
    XrString *s = XR_TO_STRING(val);
    if (len) *len = s->length;
    return s->data;
}

// Get integer field from Json
static int64_t json_get_int(XrayIsolate *X, XrJson *json, const char *key, int64_t def) {
    XrValue val = xr_json_get_by_key(X, json, key);
    if (XR_IS_INT(val)) return XR_TO_INT(val);
    if (XR_IS_FLOAT(val)) return (int64_t)XR_TO_FLOAT(val);
    return def;
}

/* ========== HTTP/2 Response Building ========== */

// Convert HTTP/2 response to Json
static XrValue h2_response_to_json(XrayIsolate *X, XrH2Response *resp) {
    XrJson *json = xr_json_new(xr_current_coro(X), 4);
    
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
 * http.h2Get(url: string) -> Json
 */
XrValue h2_get(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }
    
    const char *url = get_cstring(args[0], NULL);
    XrH2Response *resp = xr_h2_get(url);
    
    if (!resp) return xr_null();
    
    XrValue result = h2_response_to_json(X, resp);
    xr_h2_response_free(resp);
    return result;
}

/*
 * http.h2Post(url: string, body: string, contentType?: string) -> Json
 */
XrValue h2_post(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }
    
    const char *url = get_cstring(args[0], NULL);
    size_t body_len = 0;
    const char *body = get_cstring(args[1], &body_len);
    const char *content_type = argc > 2 ? get_cstring(args[2], NULL) : "application/json";
    
    XrH2Response *resp = xr_h2_post(url, body, body_len, content_type);
    
    if (!resp) return xr_null();
    
    XrValue result = h2_response_to_json(X, resp);
    xr_h2_response_free(resp);
    return result;
}

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
XrValue h2_request(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !xr_value_is_json(args[0])) {
        return xr_null();
    }
    
    XrJson *opts = xr_value_to_json(args[0]);
    
    const char *url = json_get_string(X, opts, "url", NULL);
    if (!url) return xr_null();
    
    XrH2Request req = {0};
    req.method = json_get_string(X, opts, "method", NULL);
    req.body = json_get_string(X, opts, "body", &req.body_len);
    
    XrH2Response *resp = xr_h2_request(url, &req);
    
    if (!resp) return xr_null();
    
    XrValue result = h2_response_to_json(X, resp);
    xr_h2_response_free(resp);
    return result;
}

/* ========== HTTP/2 Server Request Callback ========== */

// Set current_h2_ctx so h2Push can access the active request context
static void h2_request_callback(XrH2Context *ctx, void *user_data) {
    XrHttpContext *http_ctx = (XrHttpContext *)user_data;
    if (http_ctx) {
        http_ctx->current_h2_ctx = ctx;
    }
    // Request processing happens via xray script handler (if registered).
    // After the callback returns, clear the context.
    if (http_ctx) {
        http_ctx->current_h2_ctx = NULL;
    }
}

/* ========== HTTP/2 Server Binding (managed via XrHttpContext) ========== */

/*
 * http.h2CreateServer(options: Json) -> bool
 * 
 * options: {
 *   port?: int,
 *   cert?: string,
 *   key?: string
 * }
 */
XrValue h2_create_server(XrayIsolate *X, XrValue *args, int argc) {
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx) return xr_bool(false);
    
    XrH2ServerConfig config;
    xr_h2_server_config_init(&config);
    
    if (argc > 0 && xr_value_is_json(args[0])) {
        XrJson *opts = xr_value_to_json(args[0]);
        
        config.port = (int)json_get_int(X, opts, "port", 8443);
        config.cert_file = json_get_string(X, opts, "cert", NULL);
        config.key_file = json_get_string(X, opts, "key", NULL);
    }
    
    ctx->h2_server = xr_h2_server_new(&config);
    if (ctx->h2_server) {
        xr_h2_server_on_request(ctx->h2_server, h2_request_callback, ctx);
    }
    return xr_bool(ctx->h2_server != NULL);
}

/*
 * http.h2Listen() -> bool
 */
XrValue h2_server_listen(XrayIsolate *X, XrValue *args, int argc) {
    (void)args;
    (void)argc;
    
    XrHttpContext *ctx = xr_http_get_context(X);
    if (!ctx || !ctx->h2_server) return xr_bool(0);
    
    int ret = xr_h2_server_listen(ctx->h2_server);
    return xr_bool(ret == 0);
}

/*
 * http.h2Stop() -> void
 */
XrValue h2_server_stop(XrayIsolate *X, XrValue *args, int argc) {
    (void)args;
    (void)argc;
    
    XrHttpContext *ctx = xr_http_get_context(X);
    if (ctx && ctx->h2_server) {
        xr_h2_server_stop(ctx->h2_server);
        xr_h2_server_free(ctx->h2_server);
        ctx->h2_server = NULL;
    }
    return xr_null();
}

/*
 * http.h2Push(path: string, contentType: string, data: string) -> bool
 */
XrValue h2_push(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 3) return xr_bool(0);
    
    // Get current HTTP/2 request context from HttpContext
    XrHttpContext *http_ctx = xr_http_get_context(X);
    if (!http_ctx || !http_ctx->current_h2_ctx) return xr_bool(0);
    
    size_t path_len = 0, ct_len = 0, data_len = 0;
    const char *path = get_cstring(args[0], &path_len);
    const char *content_type = get_cstring(args[1], &ct_len);
    const char *data = get_cstring(args[2], &data_len);
    
    if (!path || !content_type || !data) return xr_bool(0);
    
    int ret = xr_h2_ctx_push(http_ctx->current_h2_ctx, path, content_type, data, data_len);
    return xr_bool(ret == 0);
}
