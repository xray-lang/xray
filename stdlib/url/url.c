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
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ========== External Declarations ========== */

extern struct XrCoroutine *xr_current_coro(XrayIsolate *X);

/* ========== Helpers ========== */

static const char hex_chars[] = "0123456789ABCDEF";

static int hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// RFC 3986: unreserved = ALPHA / DIGIT / '-' / '.' / '_' / '~'
// isalnum() is locale-sensitive (Turkish 'I' bug, etc.), so we test the
// ASCII ranges explicitly.
static inline bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

static XrValue make_str(XrayIsolate *X, const char *s, size_t len) {
    if (!s || len == 0)
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    return xr_string_value(xr_string_intern(X, s, len, 0));
}

static XrValue make_cstr(XrayIsolate *X, const char *s) {
    if (!s)
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    return xr_string_value(xr_string_intern(X, s, strlen(s), 0));
}

// Finalize an XrCtxBuf into a pooled XrValue and release the buffer. Used
// as a single exit step from the url_* binding helpers.
static XrValue ctxbuf_to_value(XrayIsolate *X, XrCtxBuf *b) {
    XrValue v = make_str(X, b->data ? b->data : "", (int) b->len);
    xr_ctxbuf_free(b);
    return v;
}

/* ========== RFC 3986 Encoding/Decoding ========== */

int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return 0;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) str[i];
        if (is_unreserved(c)) {
            if (j + 1 >= buf_size)
                break;
            buf[j++] = c;
        } else {
            if (j + 3 >= buf_size)
                break;
            buf[j++] = '%';
            buf[j++] = hex_chars[(c >> 4) & 0xF];
            buf[j++] = hex_chars[c & 0xF];
        }
    }
    buf[j] = '\0';
    return (int) j;
}

int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return 0;
    size_t j = 0;
    for (size_t i = 0; i < len && j < buf_size - 1; i++) {
        if (str[i] == '%' && i + 2 < len) {
            int hi = hex_digit(str[i + 1]);
            int lo = hex_digit(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[j++] = (char) ((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[j++] = str[i];
    }
    buf[j] = '\0';
    return (int) j;
}

/* ========== Form Encoding/Decoding ========== */

int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return 0;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) str[i];
        if (c == ' ') {
            if (j + 1 >= buf_size)
                break;
            buf[j++] = '+';
        } else if (is_unreserved(c)) {
            if (j + 1 >= buf_size)
                break;
            buf[j++] = c;
        } else {
            if (j + 3 >= buf_size)
                break;
            buf[j++] = '%';
            buf[j++] = hex_chars[(c >> 4) & 0xF];
            buf[j++] = hex_chars[c & 0xF];
        }
    }
    buf[j] = '\0';
    return (int) j;
}

int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return 0;
    size_t j = 0;
    for (size_t i = 0; i < len && j < buf_size - 1; i++) {
        if (str[i] == '%' && i + 2 < len) {
            int hi = hex_digit(str[i + 1]);
            int lo = hex_digit(str[i + 2]);
            if (hi >= 0 && lo >= 0) {
                buf[j++] = (char) ((hi << 4) | lo);
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
    return (int) j;
}

/* ========== URL Parsing (internal) ========== */

typedef struct {
    const char *protocol;
    size_t protocol_len;  // e.g. "https:"
    const char *username;
    size_t username_len;
    const char *password;
    size_t password_len;
    const char *hostname;
    size_t hostname_len;
    const char *port;
    size_t port_len;
    const char *pathname;
    size_t pathname_len;
    const char *search;
    size_t search_len;  // includes '?'
    const char *hash;
    size_t hash_len;  // includes '#'
} UrlParts;

static void url_parse_internal(const char *url, size_t url_len, UrlParts *out) {
    memset(out, 0, sizeof(*out));
    if (!url || url_len == 0)
        return;

    const char *p = url;
    const char *end = url + url_len;

    // 1. Parse protocol (scheme)
    const char *colon = memchr(p, ':', end - p);
    if (colon && colon + 2 < end && colon[1] == '/' && colon[2] == '/') {
        out->protocol = p;
        out->protocol_len = colon - p + 1;  // includes ':'
        p = colon + 3;                      // skip "://"
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
        if (*c == '@') {
            at = c;
            break;
        }
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
            if (*c == ':')
                port_colon = c;
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
            if (*c == '?' || *c == '#') {
                path_end = c;
                break;
            }
        }
        out->pathname = p;
        out->pathname_len = path_end - p;
        p = path_end;
    }

    // 5. Parse search (query string)
    if (p < end && *p == '?') {
        const char *search_end = end;
        for (const char *c = p; c < end; c++) {
            if (*c == '#') {
                search_end = c;
                break;
            }
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

static XrValue url_decode_fn(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue url_encode_form_fn(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue url_decode_form_fn(XrayIsolate *X, XrValue *args, int nargs) {
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

static XrValue url_parse_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *url_str = XR_TO_STRING(args[0]);

    UrlParts parts;
    url_parse_internal(XR_STRING_CHARS(url_str), url_str->length, &parts);

    XrJson *json = xr_json_new(xr_current_coro(X), 10);
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
    bool port_is_valid = false;
    if (parts.port && parts.port_len > 0 && parts.port_len <= 5) {
        uint32_t port_val = 0;
        port_is_valid = true;
        for (size_t i = 0; i < parts.port_len; i++) {
            char c = parts.port[i];
            if (c < '0' || c > '9') {
                port_is_valid = false;
                break;
            }
            port_val = port_val * 10 + (uint32_t) (c - '0');
        }
        if (port_is_valid && port_val > 65535)
            port_is_valid = false;
    }
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

static XrValue url_format_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return XR_NULL_VAL;
    XrJson *json = xr_value_to_json(args[0]);

    XrValue protocol = xr_json_get_by_key(X, json, "protocol");
    XrValue hostname = xr_json_get_by_key(X, json, "hostname");
    XrValue port = xr_json_get_by_key(X, json, "port");
    XrValue pathname = xr_json_get_by_key(X, json, "pathname");
    XrValue search = xr_json_get_by_key(X, json, "search");
    XrValue hash = xr_json_get_by_key(X, json, "hash");
    XrValue username = xr_json_get_by_key(X, json, "username");
    XrValue password = xr_json_get_by_key(X, json, "password");

    XrCtxBuf buf;
    xr_ctxbuf_init(&buf, 128);

    // protocol
    if (XR_IS_STRING(protocol) && XR_TO_STRING(protocol)->length > 0) {
        xr_ctxbuf_appendf(&buf, "%s//", XR_TO_STRING(protocol)->data);
    }

    // userinfo
    if (XR_IS_STRING(username) && XR_TO_STRING(username)->length > 0) {
        xr_ctxbuf_append_cstr(&buf, XR_TO_STRING(username)->data);
        if (XR_IS_STRING(password) && XR_TO_STRING(password)->length > 0) {
            xr_ctxbuf_appendf(&buf, ":%s", XR_TO_STRING(password)->data);
        }
        xr_ctxbuf_putc(&buf, '@');
    }

    // hostname
    if (XR_IS_STRING(hostname) && XR_TO_STRING(hostname)->length > 0) {
        xr_ctxbuf_append_cstr(&buf, XR_TO_STRING(hostname)->data);
    }

    // port
    if (XR_IS_STRING(port) && XR_TO_STRING(port)->length > 0) {
        xr_ctxbuf_appendf(&buf, ":%s", XR_TO_STRING(port)->data);
    }

    // pathname
    if (XR_IS_STRING(pathname) && XR_TO_STRING(pathname)->length > 0) {
        xr_ctxbuf_append_cstr(&buf, XR_TO_STRING(pathname)->data);
    }

    // search
    if (XR_IS_STRING(search) && XR_TO_STRING(search)->length > 0) {
        xr_ctxbuf_append_cstr(&buf, XR_TO_STRING(search)->data);
    }

    // hash
    if (XR_IS_STRING(hash) && XR_TO_STRING(hash)->length > 0) {
        xr_ctxbuf_append_cstr(&buf, XR_TO_STRING(hash)->data);
    }

    return ctxbuf_to_value(X, &buf);
}

static XrValue url_parse_query_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !XR_IS_STRING(args[0]))
        return XR_NULL_VAL;
    XrString *qs = XR_TO_STRING(args[0]);
    const char *str = XR_STRING_CHARS(qs);
    size_t len = qs->length;

    // Skip leading '?'
    if (len > 0 && str[0] == '?') {
        str++;
        len--;
    }

    XrJson *json = xr_json_new(xr_current_coro(X), 8);
    if (!json)
        return XR_NULL_VAL;
    if (len == 0)
        return xr_json_value(json);

    // Temp buffer for decoding
    char *dec_buf = xr_malloc(len + 1);
    if (!dec_buf)
        return xr_json_value(json);

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
            // Decode key. Almost all query-string keys fit comfortably in a
            // small stack buffer; only fall back to xr_malloc for oversize
            // keys, and abort loudly on OOM so the caller does not silently
            // receive a partial map. (Previously the entry was dropped.)
            int dk = xr_url_decode_form(key_start, key_len, dec_buf, len + 1);
            char key_small[256];
            char *key_copy = NULL;
            bool key_heap = false;
            if (dk + 1 <= (int) sizeof(key_small)) {
                memcpy(key_small, dec_buf, dk);
                key_small[dk] = '\0';
                key_copy = key_small;
            } else {
                key_copy = xr_malloc((size_t) dk + 1);
                XR_CHECK(key_copy != NULL, "url.parseQuery: OOM allocating key buffer");
                memcpy(key_copy, dec_buf, dk);
                key_copy[dk] = '\0';
                key_heap = true;
            }

            XrValue val;
            if (val_start) {
                int dv = xr_url_decode_form(val_start, val_len, dec_buf, len + 1);
                val = make_str(X, dec_buf, dv);
            } else {
                val = make_cstr(X, "");
            }

            xr_json_set_by_key(X, json, key_copy, val);
            if (key_heap)
                xr_free(key_copy);
        }

        p = amp ? amp + 1 : end;
    }

    xr_free(dec_buf);
    return xr_json_value(json);
}

// Percent-encode `src` into `buf`, growing it as needed. The worst-case
// expansion for form-encoding is 3x (each byte becomes "%HH") so the
// reservation headroom below is exact.
static void ctxbuf_append_url_form(XrCtxBuf *buf, const char *src, size_t src_len) {
    if (src_len == 0)
        return;
    xr_ctxbuf_reserve(buf, src_len * 3);
    int written = xr_url_encode_form(src, src_len, buf->data + buf->len, buf->cap - buf->len);
    if (written > 0)
        buf->len += (size_t) written;
    buf->data[buf->len] = '\0';
}

static XrValue url_build_query_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1 || !xr_value_is_json(args[0]))
        return XR_NULL_VAL;
    XrJson *json = xr_value_to_json(args[0]);

    XrShape *shape = xr_json_shape(X, json);
    if (!shape || shape->field_count == 0)
        return make_cstr(X, "");

    XrSymbolTable *st = X->symbol_table;
    XrCtxBuf buf;
    xr_ctxbuf_init(&buf, 128);

    for (uint16_t i = 0; i < shape->field_count; i++) {
        const char *key_name = xr_symbol_get_name_in_table(st, shape->field_symbols[i]);
        if (!key_name)
            continue;

        if (buf.len > 0)
            xr_ctxbuf_putc(&buf, '&');

        ctxbuf_append_url_form(&buf, key_name, strlen(key_name));

        XrValue val = xr_json_get_field_any(X, json, i);
        if (XR_IS_STRING(val)) {
            XrString *vs = XR_TO_STRING(val);
            xr_ctxbuf_putc(&buf, '=');
            ctxbuf_append_url_form(&buf, XR_STRING_CHARS(vs), vs->length);
        } else if (!XR_IS_NULL(val)) {
            // Stringify non-string primitives (int, float, bool) so
            // buildQuery({page: 1}) produces "page=1" not "page=".
            XrString *vs = xr_value_to_string(X, val);
            xr_ctxbuf_putc(&buf, '=');
            if (vs)
                ctxbuf_append_url_form(&buf, XR_STRING_CHARS(vs), vs->length);
        }
    }

    return ctxbuf_to_value(X, &buf);
}

// RFC 3986 §5.2.4: Remove dot segments from path in-place
static int remove_dot_segments(char *path, int len) {
    if (len <= 0)
        return 0;
    char *out = path;
    char *in = path;
    char *end = path + len;

    while (in < end) {
        // A: ../  or  ./
        if (in + 3 <= end && in[0] == '.' && in[1] == '.' && in[2] == '/') {
            in += 3;
            continue;
        }
        if (in + 2 <= end && in[0] == '.' && in[1] == '/') {
            in += 2;
            continue;
        }
        // B: /./  or  /. (end)
        if (in + 3 <= end && in[0] == '/' && in[1] == '.' && in[2] == '/') {
            in += 2;
            continue;
        }
        if (in + 2 == end && in[0] == '/' && in[1] == '.') {
            *out++ = '/';
            in = end;
            continue;
        }
        // C: /../  or  /.. (end)
        if (in + 4 <= end && in[0] == '/' && in[1] == '.' && in[2] == '.' && in[3] == '/') {
            in += 3;
            // Remove last segment from output
            if (out > path) {
                out--;
                while (out > path && *out != '/')
                    out--;
            }
            continue;
        }
        if (in + 3 == end && in[0] == '/' && in[1] == '.' && in[2] == '.') {
            in = end;
            if (out > path) {
                out--;
                while (out > path && *out != '/')
                    out--;
            }
            *out++ = '/';
            continue;
        }
        // D: bare . or ..
        if ((in + 1 == end && in[0] == '.') || (in + 2 == end && in[0] == '.' && in[1] == '.')) {
            in = end;
            continue;
        }
        // E: copy segment
        if (*in == '/')
            *out++ = *in++;
        while (in < end && *in != '/')
            *out++ = *in++;
    }
    *out = '\0';
    return (int) (out - path);
}

// Emit `scheme://authority` prefix from the parsed base URL into `out`.
// Used to share the header-construction logic across every branch of the
// RFC 3986 §5.3 reference-resolution algorithm implemented below.
static void url_emit_base_authority(XrCtxBuf *out, const UrlParts *bp) {
    if (bp->protocol && bp->protocol_len > 0) {
        xr_ctxbuf_appendf(out, "%.*s//", (int) bp->protocol_len, bp->protocol);
    }
    if (bp->hostname && bp->hostname_len > 0) {
        xr_ctxbuf_append(out, bp->hostname, bp->hostname_len);
    }
    if (bp->port && bp->port_len > 0) {
        xr_ctxbuf_appendf(out, ":%.*s", (int) bp->port_len, bp->port);
    }
}

// Faithful implementation of RFC 3986 §5.3 "Reference Resolution". Handles
// the reference-transforming rules from §5.2.2 including fragment-only
// references, empty paths, and the query-retention corner cases that the
// previous ad-hoc implementation silently mishandled.
static XrValue url_resolve_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 2 || !XR_IS_STRING(args[0]) || !XR_IS_STRING(args[1]))
        return XR_NULL_VAL;

    XrString *base_str = XR_TO_STRING(args[0]);
    XrString *rel_str = XR_TO_STRING(args[1]);
    const char *base = XR_STRING_CHARS(base_str);
    const char *rel = XR_STRING_CHARS(rel_str);
    size_t rel_len = rel_str->length;

    // If the reference already has a scheme (scheme://...), it is treated
    // as an absolute URI per §5.2.2 step 1 and returned unchanged.
    const char *colon = memchr(rel, ':', rel_len);
    if (colon && colon + 2 < rel + rel_len && colon[1] == '/' && colon[2] == '/') {
        return args[1];
    }

    UrlParts bp;
    url_parse_internal(base, base_str->length, &bp);

    // Split the reference into path / ?query / #fragment segments so each
    // component can be assembled independently.
    const char *rel_hash = memchr(rel, '#', rel_len);
    size_t rel_hash_len = rel_hash ? (size_t) ((rel + rel_len) - rel_hash) : 0;
    size_t rel_no_hash_len = rel_hash ? (size_t) (rel_hash - rel) : rel_len;

    const char *rel_query = memchr(rel, '?', rel_no_hash_len);
    size_t rel_query_len = 0;
    size_t rel_path_len = rel_no_hash_len;
    if (rel_query) {
        rel_query_len = (size_t) ((rel + rel_no_hash_len) - rel_query);
        rel_path_len = (size_t) (rel_query - rel);
    }

    XrCtxBuf result;
    xr_ctxbuf_init(&result, 128);

    // Track where the path portion lives in the result buffer so
    // remove_dot_segments operates only on the path, never on
    // the scheme, authority, query, or fragment.
    size_t path_start = 0, path_end = 0;

    if (rel_len > 1 && rel[0] == '/' && rel[1] == '/') {
        // Network-path reference: "//host/...": keep base scheme only.
        if (bp.protocol && bp.protocol_len > 0) {
            xr_ctxbuf_appendf(&result, "%.*s", (int) bp.protocol_len, bp.protocol);
        }
        // Append reference authority: skip "//" then find path start.
        xr_ctxbuf_append(&result, rel, 2);  // "//"
        size_t a = 2;
        while (a < rel_no_hash_len && rel[a] != '/' && rel[a] != '?')
            a++;
        xr_ctxbuf_append(&result, rel + 2, a - 2);  // authority
        path_start = result.len;
        xr_ctxbuf_append(&result, rel + a, rel_path_len > a ? rel_path_len - a : 0);
        path_end = result.len;
        if (rel_query)
            xr_ctxbuf_append(&result, rel_query, rel_query_len);
        if (rel_hash)
            xr_ctxbuf_append(&result, rel_hash, rel_hash_len);
    } else if (rel_path_len > 0 && rel[0] == '/') {
        // Absolute-path reference: keep base authority, take reference path.
        url_emit_base_authority(&result, &bp);
        path_start = result.len;
        xr_ctxbuf_append(&result, rel, rel_path_len);
        path_end = result.len;
        if (rel_query)
            xr_ctxbuf_append(&result, rel_query, rel_query_len);
        if (rel_hash)
            xr_ctxbuf_append(&result, rel_hash, rel_hash_len);
    } else if (rel_path_len == 0) {
        // Empty-path reference (query-only and/or fragment-only).
        url_emit_base_authority(&result, &bp);
        path_start = result.len;
        if (bp.pathname && bp.pathname_len > 0) {
            xr_ctxbuf_append(&result, bp.pathname, bp.pathname_len);
        }
        path_end = result.len;
        if (rel_query) {
            xr_ctxbuf_append(&result, rel_query, rel_query_len);
        } else if (bp.search && bp.search_len > 0) {
            // Preserve base query when the reference has none. (§5.2.2: if
            // reference.query is defined, use it; otherwise use base.query.)
            xr_ctxbuf_append(&result, bp.search, bp.search_len);
        }
        if (rel_hash)
            xr_ctxbuf_append(&result, rel_hash, rel_hash_len);
    } else {
        // Relative-path reference: merge base path with reference path.
        url_emit_base_authority(&result, &bp);
        path_start = result.len;

        size_t merge_prefix = 0;
        if (bp.pathname && bp.pathname_len > 0) {
            // Merge rule from §5.2.3: retain everything in base.path up to
            // and including the last '/'. If base has no path separators
            // we start from scratch with a single leading '/'.
            const char *last_slash = NULL;
            for (size_t i = bp.pathname_len; i > 0; i--) {
                if (bp.pathname[i - 1] == '/') {
                    last_slash = &bp.pathname[i - 1];
                    break;
                }
            }
            if (last_slash) {
                merge_prefix = (size_t) (last_slash - bp.pathname + 1);
                xr_ctxbuf_append(&result, bp.pathname, merge_prefix);
            } else {
                xr_ctxbuf_putc(&result, '/');
            }
        } else {
            xr_ctxbuf_putc(&result, '/');
        }
        (void) merge_prefix;
        xr_ctxbuf_append(&result, rel, rel_path_len);
        path_end = result.len;
        if (rel_query)
            xr_ctxbuf_append(&result, rel_query, rel_query_len);
        if (rel_hash)
            xr_ctxbuf_append(&result, rel_hash, rel_hash_len);
    }

    // Apply remove_dot_segments only to the path portion.
    // The helper works in-place and never grows, so operating directly
    // on the buffer storage is safe. Shift the query+fragment tail
    // leftward by the number of bytes the path shrank.
    if (path_end > path_start) {
        size_t old_path_len = path_end - path_start;
        int new_path_len = remove_dot_segments(result.data + path_start, (int) old_path_len);
        if (new_path_len < 0)
            new_path_len = 0;
        size_t shrink = old_path_len - (size_t) new_path_len;
        if (shrink > 0) {
            size_t tail_len = result.len - path_end;
            if (tail_len > 0)
                memmove(result.data + path_start + new_path_len, result.data + path_end, tail_len);
            result.len -= shrink;
        }
    }
    if (result.data)
        result.data[result.len] = '\0';
    return ctxbuf_to_value(X, &result);
}

static XrValue url_join_fn(XrayIsolate *X, XrValue *args, int nargs) {
    if (nargs < 1)
        return make_cstr(X, "");

    XrCtxBuf buf;
    xr_ctxbuf_init(&buf, 128);

    for (int i = 0; i < nargs; i++) {
        if (!XR_IS_STRING(args[i]))
            continue;
        XrString *s = XR_TO_STRING(args[i]);
        const char *seg = XR_STRING_CHARS(s);
        size_t seg_len = s->length;
        if (seg_len == 0)
            continue;

        // Remove trailing slash from current result
        if (buf.len > 0 && buf.data[buf.len - 1] == '/')
            buf.len--;

        // Add separator if needed
        if (buf.len > 0 && seg[0] != '/') {
            xr_ctxbuf_putc(&buf, '/');
        }

        xr_ctxbuf_append(&buf, seg, seg_len);
    }

    if (buf.data)
        buf.data[buf.len] = '\0';
    return ctxbuf_to_value(X, &buf);
}

/* ========== Type Declarations (parsed by gen_stdlib_types.py) ========== */

#include "../../src/module/xbuiltin_decl.h"

// @module url

XR_DEFINE_BUILTIN(url_encode_fn, "encode", "(s: string): string", "RFC 3986 percent-encode")
XR_DEFINE_BUILTIN(url_decode_fn, "decode", "(s: string): string", "RFC 3986 percent-decode")
XR_DEFINE_BUILTIN(url_encode_form_fn, "encodeForm", "(s: string): string",
                  "Form URL encode (space as +)")
XR_DEFINE_BUILTIN(url_decode_form_fn, "decodeForm", "(s: string): string",
                  "Form URL decode (+ as space)")
XR_DEFINE_BUILTIN(url_parse_fn, "parse", "(url: string): Json", "Parse URL into Json object")
XR_DEFINE_BUILTIN(url_format_fn, "format", "(obj: Json): string",
                  "Build URL string from Json components")
XR_DEFINE_BUILTIN(url_parse_query_fn, "parseQuery", "(qs: string): Json",
                  "Parse query string to Json")
XR_DEFINE_BUILTIN(url_build_query_fn, "buildQuery", "(obj: Json): string",
                  "Build query string from Json")
XR_DEFINE_BUILTIN(url_resolve_fn, "resolve", "(base: string, relative: string): string",
                  "Resolve relative URL")
XR_DEFINE_BUILTIN(url_join_fn, "join", "(...parts: string): string", "Join URL path segments")

/* ========== Module Registration ========== */

XR_FUNC XrModule *xr_load_module_url(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "xr_load_module_url: NULL isolate");

    XrModule *mod = xr_module_create_native(X, "url");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, X, "encode", url_encode_fn);
    XRS_EXPORT(mod, X, "decode", url_decode_fn);
    XRS_EXPORT(mod, X, "encodeForm", url_encode_form_fn);
    XRS_EXPORT(mod, X, "decodeForm", url_decode_form_fn);
    XRS_EXPORT(mod, X, "parse", url_parse_fn);
    XRS_EXPORT(mod, X, "format", url_format_fn);
    XRS_EXPORT(mod, X, "parseQuery", url_parse_query_fn);
    XRS_EXPORT(mod, X, "buildQuery", url_build_query_fn);
    XRS_EXPORT(mod, X, "resolve", url_resolve_fn);
    XRS_EXPORT(mod, X, "join", url_join_fn);

    mod->loaded = true;
    return mod;
}
