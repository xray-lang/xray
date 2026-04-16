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
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/base/xmalloc.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>

/* ========== External Declarations ========== */

extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);
extern struct XrCoroutine* xr_current_coro(XrayIsolate *X);

/* ========== Helpers ========== */

static const char hex_chars[] = "0123456789ABCDEF";

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool is_unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static XrValue make_str(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0) return xr_string_value(xr_string_intern(X, "", 0, 0));
    return xr_string_value(xr_string_intern(X, s, len, 0));
}

static XrValue make_cstr(XrayIsolate *X, const char *s) {
    if (!s) return xr_string_value(xr_string_intern(X, "", 0, 0));
    return xr_string_value(xr_string_intern(X, s, strlen(s), 0));
}

// Safe snprintf wrapper: clamps n to buf_size
static int safe_append(char *buf, size_t buf_size, int n, const char *fmt, ...) {
    if ((size_t)n >= buf_size) return n;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + n, buf_size - n, fmt, ap);
    va_end(ap);
    if (written < 0) return n;
    return n + written;
}

/* ========== RFC 3986 Encoding/Decoding ========== */

int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (is_unreserved(c)) {
            if (j + 1 >= buf_size) break;
            buf[j++] = c;
        } else {
            if (j + 3 >= buf_size) break;
            buf[j++] = '%';
            buf[j++] = hex_chars[(c >> 4) & 0xF];
            buf[j++] = hex_chars[c & 0xF];
        }
    }
    buf[j] = '\0';
    return (int)j;
}

int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; i < len && j < buf_size - 1; i++) {
        if (str[i] == '%' && i + 2 < len) {
            int hi = hex_digit(str[i + 1]);
            int lo = hex_digit(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[j++] = str[i];
    }
    buf[j] = '\0';
    return (int)j;
}

/* ========== Form Encoding/Decoding ========== */

int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c == ' ') {
            if (j + 1 >= buf_size) break;
            buf[j++] = '+';
        } else if (is_unreserved(c)) {
            if (j + 1 >= buf_size) break;
            buf[j++] = c;
        } else {
            if (j + 3 >= buf_size) break;
            buf[j++] = '%';
            buf[j++] = hex_chars[(c >> 4) & 0xF];
            buf[j++] = hex_chars[c & 0xF];
        }
    }
    buf[j] = '\0';
    return (int)j;
}

int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    size_t j = 0;
    for (size_t i = 0; i < len && j < buf_size - 1; i++) {
        if (str[i] == '%' && i + 2 < len) {
            int hi = hex_digit(str[i + 1]);
            int lo = hex_digit(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[j++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (str[i] == '+') {
            buf[j++] = ' ';
        } else {
            buf[j++] = str[i];
        }
    }
    buf[j] = '\0';
    return (int)j;
}

/* ========== URL Parsing (internal) ========== */

typedef struct {
    const char *protocol;   size_t protocol_len;    // e.g. "https:"
    const char *username;   size_t username_len;
    const char *password;   size_t password_len;
    const char *hostname;   size_t hostname_len;
    const char *port;       size_t port_len;
    const char *pathname;   size_t pathname_len;
    const char *search;     size_t search_len;      // includes '?'
    const char *hash;       size_t hash_len;        // includes '#'
} UrlParts;

static void url_parse_internal(const char *url, size_t url_len, UrlParts *out) {
    memset(out, 0, sizeof(*out));
    if (!url || url_len == 0) return;

    const char *p = url;
    const char *end = url + url_len;

    // 1. Parse protocol (scheme)
    const char *colon = memchr(p, ':', end - p);
    if (colon && colon + 2 < end && colon[1] == '/' && colon[2] == '/') {
        out->protocol = p;
        out->protocol_len = colon - p + 1;  // includes ':'
        p = colon + 3;  // skip "://"
    }

    // 2. Find authority end (first '/', '?', or '#')
    const char *authority_end = end;
    for (const char *c = p; c < end; c++) {
        if (*c == '/' || *c == '?' || *c == '#') {
            authority_end = c;
            break;
        }
    }

    // 3. Parse authority: [userinfo@]host[:port]
    const char *auth_start = p;
    const char *auth_end = authority_end;

    // Check for userinfo
    const char *at = NULL;
    for (const char *c = auth_start; c < auth_end; c++) {
        if (*c == '@') { at = c; break; }
    }

    const char *host_start;
    if (at) {
        // Parse username[:password]
        const char *user_colon = memchr(auth_start, ':', at - auth_start);
        out->username = auth_start;
        if (user_colon) {
            out->username_len = user_colon - auth_start;
            out->password = user_colon + 1;
            out->password_len = at - user_colon - 1;
        } else {
            out->username_len = at - auth_start;
        }
        host_start = at + 1;
    } else {
        host_start = auth_start;
    }

    // Parse host[:port] with IPv6 support
    if (host_start < auth_end && *host_start == '[') {
        // IPv6: [::1] or [::1]:port
        const char *bracket = memchr(host_start, ']', auth_end - host_start);
        if (bracket) {
            out->hostname = host_start;  // includes brackets
            out->hostname_len = bracket - host_start + 1;
            if (bracket + 1 < auth_end && bracket[1] == ':') {
                out->port = bracket + 2;
                out->port_len = auth_end - bracket - 2;
            }
        } else {
            out->hostname = host_start;
            out->hostname_len = auth_end - host_start;
        }
    } else {
        // Regular host — find last colon for port
        const char *port_colon = NULL;
        for (const char *c = host_start; c < auth_end; c++) {
            if (*c == ':') port_colon = c;
        }
        if (port_colon) {
            out->hostname = host_start;
            out->hostname_len = port_colon - host_start;
            out->port = port_colon + 1;
            out->port_len = auth_end - port_colon - 1;
        } else {
            out->hostname = host_start;
            out->hostname_len = auth_end - host_start;
        }
    }

    p = authority_end;

    // 4. Parse pathname
    if (p < end && *p == '/') {
        const char *path_end = end;
        for (const char *c = p; c < end; c++) {
            if (*c == '?' || *c == '#') { path_end = c; break; }
        }
        out->pathname = p;
        out->pathname_len = path_end - p;
        p = path_end;
    }

    // 5. Parse search (query string)
    if (p < end && *p == '?') {
        const char *search_end = end;
        for (const char *c = p; c < end; c++) {
            if (*c == '#') { search_end = c; break; }
        }
        out->search = p;
        out->search_len = search_end - p;
        p = search_end;
    }

    // 6. Parse hash (fragment)
    if (p < end && *p == '#') {
        out->hash = p;
        out->hash_len = end - p;
    }
}

/* ========== Module Bindings ========== */

static XrValue url_encode_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length * 3 + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf) return XR_NULL_VAL;
    int len = xr_url_encode(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_decode_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf) return XR_NULL_VAL;
    int len = xr_url_decode(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_encode_form_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length * 3 + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf) return XR_NULL_VAL;
    int len = xr_url_encode_form(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_decode_form_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) return XR_NULL_VAL;
    XrString *s = XR_TO_STRING(args[0]);
    size_t buf_size = s->length + 1;
    char *buf = xr_malloc(buf_size);
    if (!buf) return XR_NULL_VAL;
    int len = xr_url_decode_form(XR_STRING_CHARS(s), s->length, buf, buf_size);
    XrValue result = xr_string_value(xr_string_intern(X, buf, len, 0));
    xr_free(buf);
    return result;
}

static XrValue url_parse_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) return XR_NULL_VAL;
    XrString *url_str = XR_TO_STRING(args[0]);

    UrlParts parts;
    url_parse_internal(XR_STRING_CHARS(url_str), url_str->length, &parts);

    XrJson *json = xr_json_new(xr_current_coro(X), 10);
    if (!json) return XR_NULL_VAL;

    // protocol: "https:" or ""
    xr_json_set_by_key(X, json, "protocol",
        parts.protocol ? make_str(X, parts.protocol, parts.protocol_len)
                       : make_cstr(X, ""));

    // hostname: "example.com" or ""
    xr_json_set_by_key(X, json, "hostname",
        parts.hostname ? make_str(X, parts.hostname, parts.hostname_len)
                       : make_cstr(X, ""));

    // port: "8080" or ""
    xr_json_set_by_key(X, json, "port",
        parts.port ? make_str(X, parts.port, parts.port_len)
                   : make_cstr(X, ""));

    // pathname: "/path/to/page" or "/"
    if (parts.pathname && parts.pathname_len > 0) {
        xr_json_set_by_key(X, json, "pathname",
            make_str(X, parts.pathname, parts.pathname_len));
    } else {
        xr_json_set_by_key(X, json, "pathname", make_cstr(X, "/"));
    }

    // search: "?foo=bar" or ""
    xr_json_set_by_key(X, json, "search",
        parts.search ? make_str(X, parts.search, parts.search_len)
                     : make_cstr(X, ""));

    // hash: "#section" or ""
    xr_json_set_by_key(X, json, "hash",
        parts.hash ? make_str(X, parts.hash, parts.hash_len)
                   : make_cstr(X, ""));

    // username/password
    xr_json_set_by_key(X, json, "username",
        parts.username ? make_str(X, parts.username, parts.username_len)
                       : make_cstr(X, ""));
    xr_json_set_by_key(X, json, "password",
        parts.password ? make_str(X, parts.password, parts.password_len)
                       : make_cstr(X, ""));

    // Derived: host = hostname[:port]
    char host_buf[512];
    int host_len;
    if (parts.port && parts.port_len > 0) {
        host_len = snprintf(host_buf, sizeof(host_buf), "%.*s:%.*s",
            (int)parts.hostname_len, parts.hostname ? parts.hostname : "",
            (int)parts.port_len, parts.port);
    } else {
        host_len = snprintf(host_buf, sizeof(host_buf), "%.*s",
            (int)parts.hostname_len, parts.hostname ? parts.hostname : "");
    }
    if (host_len < 0) host_len = 0;
    if (host_len >= (int)sizeof(host_buf)) host_len = sizeof(host_buf) - 1;
    xr_json_set_by_key(X, json, "host", make_str(X, host_buf, host_len));

    // Derived: origin = protocol + "//" + host
    char origin_buf[1024];
    int origin_len = 0;
    if (parts.protocol && parts.protocol_len > 0) {
        origin_len = snprintf(origin_buf, sizeof(origin_buf), "%.*s//%.*s",
            (int)parts.protocol_len, parts.protocol,
            host_len, host_buf);
    } else {
        origin_len = snprintf(origin_buf, sizeof(origin_buf), "%.*s",
            host_len, host_buf);
    }
    if (origin_len < 0) origin_len = 0;
    if (origin_len >= (int)sizeof(origin_buf)) origin_len = sizeof(origin_buf) - 1;
    xr_json_set_by_key(X, json, "origin", make_str(X, origin_buf, origin_len));

    return xr_json_value(json);
}

static XrValue url_format_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0])) return XR_NULL_VAL;
    XrJson *json = xr_value_to_json(args[0]);

    XrValue protocol = xr_json_get_by_key(X, json, "protocol");
    XrValue hostname = xr_json_get_by_key(X, json, "hostname");
    XrValue port     = xr_json_get_by_key(X, json, "port");
    XrValue pathname = xr_json_get_by_key(X, json, "pathname");
    XrValue search   = xr_json_get_by_key(X, json, "search");
    XrValue hash     = xr_json_get_by_key(X, json, "hash");
    XrValue username = xr_json_get_by_key(X, json, "username");
    XrValue password = xr_json_get_by_key(X, json, "password");

    char buf[4096];
    int n = 0;

    // protocol
    if (XR_IS_STRING(protocol) && XR_TO_STRING(protocol)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, "%s//", XR_TO_STRING(protocol)->data);
    }

    // userinfo
    if (XR_IS_STRING(username) && XR_TO_STRING(username)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, "%s", XR_TO_STRING(username)->data);
        if (XR_IS_STRING(password) && XR_TO_STRING(password)->length > 0) {
            n = safe_append(buf, sizeof(buf), n, ":%s", XR_TO_STRING(password)->data);
        }
        n = safe_append(buf, sizeof(buf), n, "@");
    }

    // hostname
    if (XR_IS_STRING(hostname) && XR_TO_STRING(hostname)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, "%s", XR_TO_STRING(hostname)->data);
    }

    // port
    if (XR_IS_STRING(port) && XR_TO_STRING(port)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, ":%s", XR_TO_STRING(port)->data);
    }

    // pathname
    if (XR_IS_STRING(pathname) && XR_TO_STRING(pathname)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, "%s", XR_TO_STRING(pathname)->data);
    }

    // search
    if (XR_IS_STRING(search) && XR_TO_STRING(search)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, "%s", XR_TO_STRING(search)->data);
    }

    // hash
    if (XR_IS_STRING(hash) && XR_TO_STRING(hash)->length > 0) {
        n = safe_append(buf, sizeof(buf), n, "%s", XR_TO_STRING(hash)->data);
    }

    return make_str(X, buf, n);
}

static XrValue url_parse_query_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0])) return XR_NULL_VAL;
    XrString *qs = XR_TO_STRING(args[0]);
    const char *str = XR_STRING_CHARS(qs);
    size_t len = qs->length;

    // Skip leading '?'
    if (len > 0 && str[0] == '?') { str++; len--; }

    XrJson *json = xr_json_new(xr_current_coro(X), 8);
    if (!json) return XR_NULL_VAL;
    if (len == 0) return xr_json_value(json);

    // Temp buffer for decoding
    char *dec_buf = xr_malloc(len + 1);
    if (!dec_buf) return xr_json_value(json);

    const char *p = str;
    const char *end = str + len;

    while (p < end) {
        // Find next '&'
        const char *amp = memchr(p, '&', end - p);
        const char *pair_end = amp ? amp : end;

        // Find '=' in pair
        const char *eq = memchr(p, '=', pair_end - p);

        const char *key_start = p;
        size_t key_len;
        const char *val_start;
        size_t val_len;

        if (eq) {
            key_len = eq - key_start;
            val_start = eq + 1;
            val_len = pair_end - val_start;
        } else {
            key_len = pair_end - key_start;
            val_start = NULL;
            val_len = 0;
        }

        if (key_len > 0) {
            // Decode key
            int dk = xr_url_decode_form(key_start, key_len, dec_buf, len + 1);
            char *key_copy = xr_malloc(dk + 1);
            if (key_copy) {
                memcpy(key_copy, dec_buf, dk);
                key_copy[dk] = '\0';

                // Decode value
                XrValue val;
                if (val_start) {
                    int dv = xr_url_decode_form(val_start, val_len, dec_buf, len + 1);
                    val = make_str(X, dec_buf, dv);
                } else {
                    val = make_cstr(X, "");
                }

                xr_json_set_by_key(X, json, key_copy, val);
                xr_free(key_copy);
            }
        }

        p = amp ? amp + 1 : end;
    }

    xr_free(dec_buf);
    return xr_json_value(json);
}

static XrValue url_build_query_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0])) return XR_NULL_VAL;
    XrJson *json = xr_value_to_json(args[0]);

    XrShape *shape = xr_json_shape(json);
    if (!shape || shape->field_count == 0) return make_cstr(X, "");

    XrSymbolTable *st = X->symbol_table;
    char buf[4096];
    int n = 0;
    char enc_buf[1024];

    for (uint16_t i = 0; i < shape->field_count; i++) {
        const char *key_name = xr_symbol_get_name_in_table(st, shape->field_symbols[i]);
        if (!key_name) continue;

        if (n > 0 && (size_t)n < sizeof(buf)) buf[n++] = '&';

        int ek = xr_url_encode_form(key_name, strlen(key_name), enc_buf, sizeof(enc_buf));
        n = safe_append(buf, sizeof(buf), n, "%.*s", ek, enc_buf);

        XrValue val = xr_json_get_field_any(json, i);
        if (XR_IS_STRING(val)) {
            XrString *vs = XR_TO_STRING(val);
            int ev = xr_url_encode_form(XR_STRING_CHARS(vs), vs->length, enc_buf, sizeof(enc_buf));
            n = safe_append(buf, sizeof(buf), n, "=%.*s", ev, enc_buf);
        } else if (!XR_IS_NULL(val)) {
            n = safe_append(buf, sizeof(buf), n, "=");
        }
    }

    return make_str(X, buf, n);
}

// RFC 3986 §5.2.4: Remove dot segments from path in-place
static int remove_dot_segments(char *path, int len) {
    if (len <= 0) return 0;
    char *out = path;
    char *in = path;
    char *end = path + len;
    
    while (in < end) {
        // A: ../  or  ./
        if (in + 3 <= end && in[0] == '.' && in[1] == '.' && in[2] == '/') {
            in += 3; continue;
        }
        if (in + 2 <= end && in[0] == '.' && in[1] == '/') {
            in += 2; continue;
        }
        // B: /./  or  /. (end)
        if (in + 3 <= end && in[0] == '/' && in[1] == '.' && in[2] == '/') {
            in += 2; continue;
        }
        if (in + 2 == end && in[0] == '/' && in[1] == '.') {
            *out++ = '/'; in = end; continue;
        }
        // C: /../  or  /.. (end)
        if (in + 4 <= end && in[0] == '/' && in[1] == '.' && in[2] == '.' && in[3] == '/') {
            in += 3;
            // Remove last segment from output
            if (out > path) { out--; while (out > path && *out != '/') out--; }
            continue;
        }
        if (in + 3 == end && in[0] == '/' && in[1] == '.' && in[2] == '.') {
            in = end;
            if (out > path) { out--; while (out > path && *out != '/') out--; }
            *out++ = '/';
            continue;
        }
        // D: bare . or ..
        if ((in + 1 == end && in[0] == '.') ||
            (in + 2 == end && in[0] == '.' && in[1] == '.')) {
            in = end; continue;
        }
        // E: copy segment
        if (*in == '/') *out++ = *in++;
        while (in < end && *in != '/') *out++ = *in++;
    }
    *out = '\0';
    return (int)(out - path);
}

static XrValue url_resolve_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return XR_NULL_VAL;

    XrString *base_str = XR_TO_STRING(args[0]);
    XrString *rel_str = XR_TO_STRING(args[1]);
    const char *base = XR_STRING_CHARS(base_str);
    const char *rel = XR_STRING_CHARS(rel_str);
    size_t rel_len = rel_str->length;

    // If relative is absolute URL (has scheme), return as-is
    const char *colon = memchr(rel, ':', rel_len);
    if (colon && colon + 2 < rel + rel_len && colon[1] == '/' && colon[2] == '/') {
        return args[1];
    }

    // Parse base URL
    UrlParts bp;
    url_parse_internal(base, base_str->length, &bp);

    char result[4096];
    int n = 0;

    if (rel_len > 0 && rel[0] == '/') {
        // Protocol-relative or absolute path
        if (rel_len > 1 && rel[1] == '/') {
            // Protocol-relative: //host/path
            if (bp.protocol && bp.protocol_len > 0) {
                n = safe_append(result, sizeof(result), n, "%.*s%.*s",
                    (int)bp.protocol_len, bp.protocol,
                    (int)rel_len, rel);
            } else {
                return args[1];
            }
        } else {
            // Absolute path: /path
            if (bp.protocol && bp.protocol_len > 0) {
                n = safe_append(result, sizeof(result), n, "%.*s//",
                    (int)bp.protocol_len, bp.protocol);
            }
            if (bp.hostname && bp.hostname_len > 0) {
                n = safe_append(result, sizeof(result), n, "%.*s",
                    (int)bp.hostname_len, bp.hostname);
            }
            if (bp.port && bp.port_len > 0) {
                n = safe_append(result, sizeof(result), n, ":%.*s",
                    (int)bp.port_len, bp.port);
            }
            n = safe_append(result, sizeof(result), n, "%.*s", (int)rel_len, rel);
        }
    } else {
        // Relative path: merge with base
        if (bp.protocol && bp.protocol_len > 0) {
            n = safe_append(result, sizeof(result), n, "%.*s//",
                (int)bp.protocol_len, bp.protocol);
        }
        if (bp.hostname && bp.hostname_len > 0) {
            n = safe_append(result, sizeof(result), n, "%.*s",
                (int)bp.hostname_len, bp.hostname);
        }
        if (bp.port && bp.port_len > 0) {
            n = safe_append(result, sizeof(result), n, ":%.*s",
                (int)bp.port_len, bp.port);
        }

        // Find last '/' in base pathname
        if (bp.pathname && bp.pathname_len > 0) {
            const char *last_slash = NULL;
            for (size_t i = bp.pathname_len; i > 0; i--) {
                if (bp.pathname[i - 1] == '/') { last_slash = &bp.pathname[i - 1]; break; }
            }
            if (last_slash) {
                n = safe_append(result, sizeof(result), n, "%.*s/",
                    (int)(last_slash - bp.pathname), bp.pathname);
            } else {
                n = safe_append(result, sizeof(result), n, "/");
            }
        } else {
            n = safe_append(result, sizeof(result), n, "/");
        }
        n = safe_append(result, sizeof(result), n, "%.*s", (int)rel_len, rel);
    }

    // Apply remove_dot_segments to the path portion of result
    n = remove_dot_segments(result, n);
    return make_str(X, result, n);
}

static XrValue url_join_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1) return make_cstr(X, "");

    char buf[4096];
    int n = 0;

    for (int i = 0; i < nargs; i++) {
        if (!XR_IS_STRING(args[i])) continue;
        XrString *s = XR_TO_STRING(args[i]);
        const char *seg = XR_STRING_CHARS(s);
        size_t seg_len = s->length;
        if (seg_len == 0) continue;

        // Remove trailing slash from current result
        if (n > 0 && buf[n - 1] == '/') n--;

        // Add separator if needed
        if (n > 0 && seg[0] != '/') {
            n = safe_append(buf, sizeof(buf), n, "/");
        }

        n = safe_append(buf, sizeof(buf), n, "%.*s", (int)seg_len, seg);
    }

    return make_str(X, buf, n);
}

/* ========== Type Declarations (parsed by gen_stdlib_types.py) ========== */

#include "../../src/module/xbuiltin_decl.h"

// @module url

XR_DEFINE_BUILTIN(url_encode_fn, "encode", "(s: string): string", "RFC 3986 percent-encode")
XR_DEFINE_BUILTIN(url_decode_fn, "decode", "(s: string): string", "RFC 3986 percent-decode")
XR_DEFINE_BUILTIN(url_encode_form_fn, "encodeForm", "(s: string): string", "Form URL encode (space as +)")
XR_DEFINE_BUILTIN(url_decode_form_fn, "decodeForm", "(s: string): string", "Form URL decode (+ as space)")
XR_DEFINE_BUILTIN(url_parse_fn, "parse", "(url: string): Json", "Parse URL into Json object")
XR_DEFINE_BUILTIN(url_format_fn, "format", "(obj: Json): string", "Build URL string from Json components")
XR_DEFINE_BUILTIN(url_parse_query_fn, "parseQuery", "(qs: string): Json", "Parse query string to Json")
XR_DEFINE_BUILTIN(url_build_query_fn, "buildQuery", "(obj: Json): string", "Build query string from Json")
XR_DEFINE_BUILTIN(url_resolve_fn, "resolve", "(base: string, relative: string): string", "Resolve relative URL")
XR_DEFINE_BUILTIN(url_join_fn, "join", "(...parts: string): string", "Join URL path segments")

/* ========== Module Registration ========== */

static void register_fn(XrayIsolate *X, XrModule *mod, XrCFunctionPtr func, const char *name) {
    XrCFunction *fn = xr_vm_cfunction_new(X, func, name);
    xr_module_add_export(X, mod, name, xr_value_from_cfunction(fn));
}

XrModule* xr_load_module_url(XrayIsolate *X) {
    XrModule *mod = xr_module_create_native(X, "url");
    register_fn(X, mod, url_encode_fn,       "encode");
    register_fn(X, mod, url_decode_fn,       "decode");
    register_fn(X, mod, url_encode_form_fn,  "encodeForm");
    register_fn(X, mod, url_decode_form_fn,  "decodeForm");
    register_fn(X, mod, url_parse_fn,        "parse");
    register_fn(X, mod, url_format_fn,       "format");
    register_fn(X, mod, url_parse_query_fn,  "parseQuery");
    register_fn(X, mod, url_build_query_fn,  "buildQuery");
    register_fn(X, mod, url_resolve_fn,      "resolve");
    register_fn(X, mod, url_join_fn,         "join");
    mod->loaded = true;
    return mod;
}
