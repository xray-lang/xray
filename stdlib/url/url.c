/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * url.c - URL standard library implementation
 *
 * KEY CONCEPT:
 *   RFC 3986 compliant URL parsing, encoding, query parameter handling.
 *   parse() returns a Json object with structured fields.
 *
 * WHY THIS DESIGN:
 *   - Separate encode/decode (RFC 3986) from encodeForm/decodeForm (HTML forms)
 *   - parse() returns Json for direct field access (result.hostname, result.port)
 *   - parseQuery/buildQuery use Json for consistency with parse()
 */

#include "url.h"
#include "../common.h"
#include "../ctxbuf.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/base/xmalloc.h"
#include "../../src/base/xchecks.h"
#include "../../src/runtime/value/xvalue_format.h"
#include "../../src/shared/xr_url_core.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ========== External Declarations ========== */

extern struct XrCoroutine *xr_current_coro(XrVMRuntime *X);

/* ========== Helpers ========== */

static XrValue make_str(XrVMRuntime *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    return xr_string_value(xr_string_intern(X, s, len, 0));
}

static XrValue make_cstr(XrVMRuntime *X, const char *s) {
    if (!s)
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    return xr_string_value(xr_string_intern(X, s, strlen(s), 0));
}

// Finalize an XrCtxBuf into a pooled XrValue and release the buffer. Used
// as a single exit step from the url_* binding helpers.
static XrValue ctxbuf_to_value(XrVMRuntime *X, XrCtxBuf *b) {
    XrValue v = make_str(X, b->data ? b->data : "", (int) b->len);
    xr_ctxbuf_free(b);
    return v;
}

static bool url_core_writer_append(void *ctx, const char *data, size_t len) {
    xr_ctxbuf_append((XrCtxBuf *) ctx, data, len);
    return true;
}

static bool url_core_writer_putc(void *ctx, char c) {
    xr_ctxbuf_putc((XrCtxBuf *) ctx, c);
    return true;
}

static char *url_core_writer_data(void *ctx) {
    XrCtxBuf *buf = (XrCtxBuf *) ctx;
    return buf ? buf->data : NULL;
}

static size_t url_core_writer_len(void *ctx) {
    XrCtxBuf *buf = (XrCtxBuf *) ctx;
    return buf ? buf->len : 0;
}

static void url_core_writer_set_len(void *ctx, size_t len) {
    XrCtxBuf *buf = (XrCtxBuf *) ctx;
    if (!buf)
        return;
    buf->len = len;
    buf->data[buf->len] = '\0';
}

static XrUrlCoreWriter url_core_writer(XrCtxBuf *buf) {
    XrUrlCoreWriter writer;
    writer.ctx = buf;
    writer.append = url_core_writer_append;
    writer.putc = url_core_writer_putc;
    writer.data = url_core_writer_data;
    writer.len = url_core_writer_len;
    writer.set_len = url_core_writer_set_len;
    return writer;
}

/* ========== RFC 3986 Encoding/Decoding ========== */

int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_encode_bounded(str, len, false, buf, buf_size);
}

int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_decode_bounded(str, len, false, buf, buf_size);
}

/* ========== Form Encoding/Decoding ========== */

int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_encode_bounded(str, len, true, buf, buf_size);
}

int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    return xr_url_core_decode_bounded(str, len, true, buf, buf_size);
}

/* ========== Module Bindings ========== */

static XrValue url_encode_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length * 3 + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf)
        return XR_NULL_VAL;
    int len = xr_url_encode(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_decode_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf)
        return XR_NULL_VAL;
    int len = xr_url_decode(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_encode_form_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length * 3 + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf)
        return XR_NULL_VAL;
    int len = xr_url_encode_form(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_decode_form_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf)
        return XR_NULL_VAL;
    int len = xr_url_decode_form(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_parse_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *url_str = XR_TO_STRING(args[0]);

    XrUrlCoreParts parts;
    xr_url_core_parse(XR_STRING_CHARS(url_str), url_str->length, &parts);

    XrJson *json = xr_json_new(xr_current_coro(X));
    if (!json)
        return XR_NULL_VAL;

    // protocol: "https:" or ""
    xr_json_set_by_key(X, json, "protocol",
                       parts.protocol ? make_str(X, parts.protocol, parts.protocol_len)
                                      : make_cstr(X, ""));

    // hostname: "example.com" or ""
    xr_json_set_by_key(X, json, "hostname",
                       parts.hostname ? make_str(X, parts.hostname, parts.hostname_len)
                                      : make_cstr(X, ""));

    // port: "8080" or "". The parser accepts any byte sequence between the
    // hostname colon and the path separator, so validate that the captured
    // substring is a decimal number in the IANA-legal range [0, 65535].
    // Anything else is surfaced as an empty port, mirroring how browsers
    // treat "http://host:abc/" (the colon is ignored).
    bool port_is_valid = xr_url_core_port_is_valid(&parts);
    if (port_is_valid) {
        xr_json_set_by_key(X, json, "port", make_str(X, parts.port, parts.port_len));
    } else {
        xr_json_set_by_key(X, json, "port", make_cstr(X, ""));
    }

    // pathname: "/path/to/page" or "/"
    if (parts.pathname && parts.pathname_len > 0) {
        xr_json_set_by_key(X, json, "pathname", make_str(X, parts.pathname, parts.pathname_len));
    } else {
        xr_json_set_by_key(X, json, "pathname", make_cstr(X, "/"));
    }

    // search: "?foo=bar" or ""
    xr_json_set_by_key(X, json, "search",
                       parts.search ? make_str(X, parts.search, parts.search_len)
                                    : make_cstr(X, ""));

    // hash: "#section" or ""
    xr_json_set_by_key(X, json, "hash",
                       parts.hash ? make_str(X, parts.hash, parts.hash_len) : make_cstr(X, ""));

    // username/password
    xr_json_set_by_key(X, json, "username",
                       parts.username ? make_str(X, parts.username, parts.username_len)
                                      : make_cstr(X, ""));
    xr_json_set_by_key(X, json, "password",
                       parts.password ? make_str(X, parts.password, parts.password_len)
                                      : make_cstr(X, ""));

    // Derived: host = hostname[:port] (only include validated port)
    XrCtxBuf host_buf;
    xr_ctxbuf_init(&host_buf, 64);
    if (port_is_valid) {
        xr_ctxbuf_appendf(&host_buf, "%.*s:%.*s", (int) parts.hostname_len,
                          parts.hostname ? parts.hostname : "", (int) parts.port_len, parts.port);
    } else {
        xr_ctxbuf_appendf(&host_buf, "%.*s", (int) parts.hostname_len,
                          parts.hostname ? parts.hostname : "");
    }
    xr_json_set_by_key(X, json, "host", make_str(X, host_buf.data, (int) host_buf.len));

    // Derived: origin = protocol + "//" + host
    XrCtxBuf origin_buf;
    xr_ctxbuf_init(&origin_buf, 64);
    if (parts.protocol && parts.protocol_len > 0) {
        xr_ctxbuf_appendf(&origin_buf, "%.*s//%.*s", (int) parts.protocol_len, parts.protocol,
                          (int) host_buf.len, host_buf.data ? host_buf.data : "");
    } else {
        xr_ctxbuf_appendf(&origin_buf, "%.*s", (int) host_buf.len,
                          host_buf.data ? host_buf.data : "");
    }
    xr_json_set_by_key(X, json, "origin", make_str(X, origin_buf.data, (int) origin_buf.len));

    xr_ctxbuf_free(&host_buf);
    xr_ctxbuf_free(&origin_buf);
    return xr_json_value(json);
}

typedef struct {
    XrVMRuntime *X;
    XrJson *json;
} UrlFormatFields;

static bool url_format_field(void *ctx, const char *name, const char **data, size_t *len) {
    UrlFormatFields *fields = (UrlFormatFields *) ctx;
    if (!fields || !fields->X || !fields->json || !data || !len)
        return false;
    XrValue value = xr_json_get_by_key(fields->X, fields->json, name);
    if (XR_IS_STRING(value)) {
        XrString *s = XR_TO_STRING(value);
        *data = XR_STRING_CHARS(s);
        *len = s->length;
    } else {
        *data = NULL;
        *len = 0;
    }
    return true;
}

static XrValue url_format_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return XR_NULL_VAL;

    XrCtxBuf buf;
    xr_ctxbuf_init(&buf, 128);
    XrUrlCoreWriter writer = url_core_writer(&buf);
    UrlFormatFields fields;
    fields.X = X;
    fields.json = xr_value_to_json(args[0]);
    if (!xr_url_core_format_write(url_format_field, &fields, &writer)) {
        xr_ctxbuf_free(&buf);
        return XR_NULL_VAL;
    }
    return ctxbuf_to_value(X, &buf);
}

typedef struct {
    XrVMRuntime *X;
    XrJson *json;
} UrlParseQueryCtx;

static bool url_parse_query_pair(void *ctx, const char *key, size_t key_len, const char *value,
                                 size_t value_len, bool has_value) {
    UrlParseQueryCtx *parse_ctx = (UrlParseQueryCtx *) ctx;
    XrVMRuntime *X = parse_ctx->X;

    size_t decoded_key_len = 0;
    if (!xr_url_core_decoded_len(key, key_len, &decoded_key_len))
        return false;

    char key_small[256];
    char *key_copy = key_small;
    bool key_heap = false;
    if (decoded_key_len + 1 > sizeof(key_small)) {
        key_copy = xr_malloc(decoded_key_len + 1);
        XR_CHECK(key_copy != NULL, "url.parseQuery: OOM allocating key buffer");
        key_heap = true;
    }

    if (!xr_url_core_decode(key, key_len, true, key_copy, &decoded_key_len)) {
        if (key_heap)
            xr_free(key_copy);
        return false;
    }

    XrValue val;
    if (has_value) {
        size_t decoded_value_len = 0;
        if (!xr_url_core_decoded_len(value, value_len, &decoded_value_len)) {
            if (key_heap)
                xr_free(key_copy);
            return false;
        }

        char value_small[256];
        char *value_copy = value_small;
        bool value_heap = false;
        if (decoded_value_len + 1 > sizeof(value_small)) {
            value_copy = xr_malloc(decoded_value_len + 1);
            XR_CHECK(value_copy != NULL, "url.parseQuery: OOM allocating value buffer");
            value_heap = true;
        }

        if (!xr_url_core_decode(value, value_len, true, value_copy, &decoded_value_len)) {
            if (value_heap)
                xr_free(value_copy);
            if (key_heap)
                xr_free(key_copy);
            return false;
        }

        val = make_str(X, value_copy, decoded_value_len);
        if (value_heap)
            xr_free(value_copy);
    } else {
        val = make_cstr(X, "");
    }

    xr_json_set_by_key(X, parse_ctx->json, key_copy, val);
    if (key_heap)
        xr_free(key_copy);
    return true;
}

static XrValue url_parse_query_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *qs = XR_TO_STRING(args[0]);
    XrJson *json = xr_json_new(xr_current_coro(X));
    if (!json)
        return XR_NULL_VAL;

    UrlParseQueryCtx ctx;
    ctx.X = X;
    ctx.json = json;
    if (!xr_url_core_parse_query_each(XR_STRING_CHARS(qs), qs->length, url_parse_query_pair, &ctx))
        return XR_NULL_VAL;
    return xr_json_value(json);
}

static XrValue url_build_query_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return XR_NULL_VAL;
    XrJson *json = xr_value_to_json(args[0]);

    XrClass *cls = json->klass;
    if (!cls || cls->field_count == 0)
        return make_cstr(X, "");

    XrCtxBuf buf;
    xr_ctxbuf_init(&buf, 128);
    XrUrlCoreWriter writer = url_core_writer(&buf);
    bool has_pairs = false;

    for (uint16_t i = 0; i < cls->field_count; i++) {
        const char *key_name = cls->fields[i].name;
        if (!key_name)
            continue;

        XrValue val = xr_instance_get_dynamic_field(json, i);
        if (XR_IS_STRING(val)) {
            XrString *vs = XR_TO_STRING(val);
            if (!xr_url_core_build_query_pair_write(&writer, &has_pairs, key_name, strlen(key_name),
                                                    XR_STRING_CHARS(vs), vs->length, true)) {
                xr_ctxbuf_free(&buf);
                return XR_NULL_VAL;
            }
        } else if (!XR_IS_NULL(val)) {
            // Stringify non-string primitives (int, float, bool) so
            // buildQuery({page: 1}) produces "page=1" not "page=".
            XrString *vs = xr_value_to_string(X, val);
            const char *value_data = vs ? XR_STRING_CHARS(vs) : "";
            size_t value_len = vs ? vs->length : 0;
            if (!xr_url_core_build_query_pair_write(&writer, &has_pairs, key_name, strlen(key_name),
                                                    value_data, value_len, true)) {
                xr_ctxbuf_free(&buf);
                return XR_NULL_VAL;
            }
        } else if (!xr_url_core_build_query_pair_write(&writer, &has_pairs, key_name,
                                                       strlen(key_name), NULL, 0, false)) {
            xr_ctxbuf_free(&buf);
            return XR_NULL_VAL;
        }
    }

    return ctxbuf_to_value(X, &buf);
}

// Faithful implementation of RFC 3986 §5.3 "Reference Resolution".
// The algorithm lives in shared core; this function only adapts VM strings.
static XrValue url_resolve_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return XR_NULL_VAL;

    XrString *base_str = XR_TO_STRING(args[0]);
    XrString *rel_str = XR_TO_STRING(args[1]);

    XrCtxBuf result;
    xr_ctxbuf_init(&result, 128);
    XrUrlCoreWriter writer = url_core_writer(&result);
    if (!xr_url_core_resolve_write(XR_STRING_CHARS(base_str), base_str->length,
                                   XR_STRING_CHARS(rel_str), rel_str->length, &writer)) {
        xr_ctxbuf_free(&result);
        return XR_NULL_VAL;
    }
    return ctxbuf_to_value(X, &result);
}

typedef struct {
    XrValue *args;
    int nargs;
} UrlJoinParts;

static bool url_join_part(void *ctx, size_t index, const char **data, size_t *len) {
    UrlJoinParts *parts = (UrlJoinParts *) ctx;
    if (!parts || !data || !len || index >= (size_t) parts->nargs)
        return false;
    XrValue value = parts->args[index];
    if (!XR_IS_STRING(value)) {
        *data = NULL;
        *len = 0;
        return true;
    }
    XrString *s = XR_TO_STRING(value);
    *data = XR_STRING_CHARS(s);
    *len = s->length;
    return true;
}

static XrValue url_join_fn(XrVMRuntime *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return make_cstr(X, "");

    XrCtxBuf buf;
    xr_ctxbuf_init(&buf, 128);
    XrUrlCoreWriter writer = url_core_writer(&buf);
    UrlJoinParts parts;
    parts.args = args;
    parts.nargs = nargs;
    if (!xr_url_core_join_write((size_t) nargs, url_join_part, &parts, &writer)) {
        xr_ctxbuf_free(&buf);
        return XR_NULL_VAL;
    }
    return ctxbuf_to_value(X, &buf);
}

#define XR_STDLIB_VM_BIND_MODULE_URL 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_URL

/* ========== Module Registration ========== */

XR_FUNC XrModule *xr_load_module_url(XrVMRuntime *X) {
    XR_DCHECK(X != NULL, "xr_load_module_url: NULL isolate");

    XrModule *mod = xr_module_create_native(X, "url");
    if (!mod)
        return NULL;

    xr_stdlib_vm_bind_url_generated(X, mod);

    mod->loaded = true;
    return mod;
}
