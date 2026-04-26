/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.c - Pure C JSON parser, builder, and serializer (L0)
 *
 * KEY CONCEPT:
 *   RFC 8259-compliant single-pass JSON parser with DOM output.
 *   Parser strictness from stdlib/json + pure C DOM from xlsp_json.
 *   No runtime type dependencies — only xdefs.h + xmalloc.h.
 */

#include "xjson.h"
#include "xmalloc.h"
#include "xchecks.h"
#include "../runtime/object/xutf8.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

/* ========== Parser State ========== */

typedef struct {
    const char *src;
    const char *end;
    const char *pos;
    int depth;
} JsonParser;

/* ========== Parser Helpers ========== */

/* RFC 8259: only SP/TAB/LF/CR are JSON whitespace */
static void skip_whitespace(JsonParser *p) {
    while (p->pos < p->end) {
        char c = *p->pos;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static XrJsonValue *parse_value(JsonParser *p);

static XrJsonValue *alloc_value(XrJsonType type) {
    XrJsonValue *v = (XrJsonValue *) xr_calloc(1, sizeof(XrJsonValue));
    if (v)
        v->type = type;
    return v;
}

/* ========== Parse Null ========== */

static XrJsonValue *parse_null(JsonParser *p) {
    if (p->pos + 4 <= p->end && strncmp(p->pos, "null", 4) == 0) {
        p->pos += 4;
        return alloc_value(XR_JSON_NULL);
    }
    return NULL;
}

/* ========== Parse Bool ========== */

static XrJsonValue *parse_bool(JsonParser *p) {
    if (p->pos + 4 <= p->end && strncmp(p->pos, "true", 4) == 0) {
        p->pos += 4;
        XrJsonValue *v = alloc_value(XR_JSON_BOOL);
        if (v)
            v->as.boolean = true;
        return v;
    }
    if (p->pos + 5 <= p->end && strncmp(p->pos, "false", 5) == 0) {
        p->pos += 5;
        XrJsonValue *v = alloc_value(XR_JSON_BOOL);
        if (v)
            v->as.boolean = false;
        return v;
    }
    return NULL;
}

/* ========== Parse Number (RFC 8259 strict) ========== */

static XrJsonValue *parse_number(JsonParser *p) {
    const char *start = p->pos;

    /* Optional negative sign */
    if (p->pos < p->end && *p->pos == '-')
        p->pos++;

    /* Integer part */
    if (p->pos >= p->end || !isdigit((unsigned char) *p->pos)) {
        p->pos = start;
        return NULL;
    }

    /* RFC 8259: leading zeros not allowed (except "0" itself) */
    const char *digit_start = p->pos;
    while (p->pos < p->end && isdigit((unsigned char) *p->pos))
        p->pos++;
    int digit_count = (int) (p->pos - digit_start);
    if (digit_count > 1 && *digit_start == '0') {
        p->pos = start;
        return NULL; /* leading zero */
    }

    bool is_float = false;

    /* Fractional part: at least one digit after '.' */
    if (p->pos < p->end && *p->pos == '.') {
        is_float = true;
        p->pos++;
        if (p->pos >= p->end || !isdigit((unsigned char) *p->pos)) {
            p->pos = start;
            return NULL;
        }
        while (p->pos < p->end && isdigit((unsigned char) *p->pos))
            p->pos++;
    }

    /* Exponent part: at least one digit after e/E */
    if (p->pos < p->end && (*p->pos == 'e' || *p->pos == 'E')) {
        is_float = true;
        p->pos++;
        if (p->pos < p->end && (*p->pos == '+' || *p->pos == '-'))
            p->pos++;
        if (p->pos >= p->end || !isdigit((unsigned char) *p->pos)) {
            p->pos = start;
            return NULL;
        }
        while (p->pos < p->end && isdigit((unsigned char) *p->pos))
            p->pos++;
    }

    XrJsonValue *v = alloc_value(XR_JSON_NUMBER);
    if (!v)
        return NULL;

    if (is_float) {
        v->is_integer = false;
        v->as.number = strtod(start, NULL);
    } else {
        /* Try int64 first; fall back to double on overflow */
        errno = 0;
        char *end_ptr;
        int64_t ival = strtoll(start, &end_ptr, 10);
        if (errno == ERANGE) {
            v->is_integer = false;
            v->as.number = strtod(start, NULL);
        } else {
            v->is_integer = true;
            v->as.integer = ival;
        }
    }
    return v;
}

/* ========== Parse String (RFC 8259 strict) ========== */

/* Parse 4 hex digits, return codepoint or -1 on error */
static int parse_hex4(const char *s) {
    unsigned int val = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        val <<= 4;
        if (c >= '0' && c <= '9')
            val |= (unsigned) (c - '0');
        else if (c >= 'a' && c <= 'f')
            val |= (unsigned) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val |= (unsigned) (c - 'A' + 10);
        else
            return -1;
    }
    return (int) val;
}

static char *parse_string_content(JsonParser *p) {
    if (p->pos >= p->end || *p->pos != '"')
        return NULL;
    p->pos++; /* skip opening quote */

    /* Stack buffer for common case; heap for long strings */
    char stack_buf[256];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;

#define STR_ENSURE(n)                                                                              \
    do {                                                                                           \
        if (len + (n) >= cap) {                                                                    \
            size_t new_cap = cap * 2;                                                              \
            while (new_cap < len + (n) + 1)                                                        \
                new_cap *= 2;                                                                      \
            char *nb = (char *) xr_malloc(new_cap);                                                \
            if (!nb) {                                                                             \
                if (buf != stack_buf)                                                              \
                    xr_free(buf);                                                                  \
                return NULL;                                                                       \
            }                                                                                      \
            memcpy(nb, buf, len);                                                                  \
            if (buf != stack_buf)                                                                  \
                xr_free(buf);                                                                      \
            buf = nb;                                                                              \
            cap = new_cap;                                                                         \
        }                                                                                          \
    } while (0)

    while (p->pos < p->end && *p->pos != '"') {
        if (*p->pos == '\\') {
            p->pos++;
            if (p->pos >= p->end)
                break;
            STR_ENSURE(4); /* max UTF-8 bytes for one codepoint */
            switch (*p->pos) {
                case '"':
                    buf[len++] = '"';
                    p->pos++;
                    break;
                case '\\':
                    buf[len++] = '\\';
                    p->pos++;
                    break;
                case '/':
                    buf[len++] = '/';
                    p->pos++;
                    break;
                case 'b':
                    buf[len++] = '\b';
                    p->pos++;
                    break;
                case 'f':
                    buf[len++] = '\f';
                    p->pos++;
                    break;
                case 'n':
                    buf[len++] = '\n';
                    p->pos++;
                    break;
                case 'r':
                    buf[len++] = '\r';
                    p->pos++;
                    break;
                case 't':
                    buf[len++] = '\t';
                    p->pos++;
                    break;
                case 'u': {
                    p->pos++;
                    if (p->pos + 4 > p->end)
                        goto bad_escape;
                    int cp = parse_hex4(p->pos);
                    if (cp < 0)
                        goto bad_escape;
                    p->pos += 4;

                    /* UTF-16 surrogate pair handling */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        /* High surrogate: expect \uDCxx */
                        if (p->pos + 6 <= p->end && p->pos[0] == '\\' && p->pos[1] == 'u') {
                            int low = parse_hex4(p->pos + 2);
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                unsigned int full = 0x10000 + ((unsigned) (cp - 0xD800) << 10) +
                                                    (unsigned) (low - 0xDC00);
                                len += xr_utf8_encode(full, buf + len);
                                p->pos += 6;
                            } else {
                                goto bad_escape;
                            }
                        } else {
                            goto bad_escape;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        /* Lone low surrogate */
                        goto bad_escape;
                    } else {
                        len += xr_utf8_encode((uint32_t) cp, buf + len);
                    }
                    break;

                bad_escape:
                    /* Invalid unicode escape: free and fail */
                    if (buf != stack_buf)
                        xr_free(buf);
                    return NULL;
                }
                default:
                    /* RFC 8259: only the above escapes are valid */
                    if (buf != stack_buf)
                        xr_free(buf);
                    return NULL;
            }
        } else {
            STR_ENSURE(1);
            buf[len++] = *p->pos++;
        }
    }

#undef STR_ENSURE

    if (p->pos >= p->end || *p->pos != '"') {
        /* Unterminated string */
        if (buf != stack_buf)
            xr_free(buf);
        return NULL;
    }
    p->pos++; /* skip closing quote */

    /* Copy result to heap */
    char *result = (char *) xr_malloc(len + 1);
    if (!result) {
        if (buf != stack_buf)
            xr_free(buf);
        return NULL;
    }
    memcpy(result, buf, len);
    result[len] = '\0';
    if (buf != stack_buf)
        xr_free(buf);
    return result;
}

static XrJsonValue *parse_string(JsonParser *p) {
    char *str = parse_string_content(p);
    if (!str)
        return NULL;

    XrJsonValue *v = alloc_value(XR_JSON_STRING);
    if (!v) {
        xr_free(str);
        return NULL;
    }
    v->as.string = str;
    return v;
}

/* ========== Parse Array ========== */

static XrJsonValue *parse_array(JsonParser *p) {
    XR_DCHECK(p->pos < p->end && *p->pos == '[', "parse_array: expected '['");
    p->pos++;

    XrJsonValue *arr = alloc_value(XR_JSON_ARRAY);
    if (!arr)
        return NULL;
    arr->as.array.items = NULL;
    arr->as.array.count = 0;
    arr->as.array.capacity = 0;

    skip_whitespace(p);
    if (p->pos < p->end && *p->pos == ']') {
        p->pos++;
        return arr;
    }

    while (1) {
        skip_whitespace(p);
        XrJsonValue *elem = parse_value(p);
        if (!elem) {
            xjson_free(arr);
            return NULL;
        }

        xjson_array_push(arr, elem);

        skip_whitespace(p);
        if (p->pos < p->end && *p->pos == ']') {
            p->pos++;
            break;
        }
        if (p->pos >= p->end || *p->pos != ',') {
            xjson_free(arr);
            return NULL;
        }
        p->pos++; /* skip comma */
    }

    return arr;
}

/* ========== Parse Object ========== */

static XrJsonValue *parse_object(JsonParser *p) {
    XR_DCHECK(p->pos < p->end && *p->pos == '{', "parse_object: expected '{'");
    p->pos++;

    XrJsonValue *obj = alloc_value(XR_JSON_OBJECT);
    if (!obj)
        return NULL;
    obj->as.object.members = NULL;
    obj->as.object.count = 0;
    obj->as.object.capacity = 0;

    skip_whitespace(p);
    if (p->pos < p->end && *p->pos == '}') {
        p->pos++;
        return obj;
    }

    while (1) {
        skip_whitespace(p);

        /* Key must be a string */
        char *key = parse_string_content(p);
        if (!key) {
            xjson_free(obj);
            return NULL;
        }

        skip_whitespace(p);
        if (p->pos >= p->end || *p->pos != ':') {
            xr_free(key);
            xjson_free(obj);
            return NULL;
        }
        p->pos++;

        skip_whitespace(p);
        XrJsonValue *val = parse_value(p);
        if (!val) {
            xr_free(key);
            xjson_free(obj);
            return NULL;
        }

        /* Append member (dedup check handled by xjson_object_set) */
        xjson_object_set(obj, key, val);
        xr_free(key); /* xjson_object_set strdup's the key */

        skip_whitespace(p);
        if (p->pos < p->end && *p->pos == '}') {
            p->pos++;
            break;
        }
        if (p->pos >= p->end || *p->pos != ',') {
            xjson_free(obj);
            return NULL;
        }
        p->pos++;
    }

    return obj;
}

/* ========== Parse Value (dispatch) ========== */

static XrJsonValue *parse_value(JsonParser *p) {
    skip_whitespace(p);

    if (p->pos >= p->end)
        return NULL;

    /* Depth guard */
    if (p->depth >= XJSON_MAX_DEPTH)
        return NULL;
    p->depth++;

    XrJsonValue *result = NULL;
    switch (*p->pos) {
        case 'n':
            result = parse_null(p);
            break;
        case 't':
        case 'f':
            result = parse_bool(p);
            break;
        case '"':
            result = parse_string(p);
            break;
        case '[':
            result = parse_array(p);
            break;
        case '{':
            result = parse_object(p);
            break;
        default:
            if (*p->pos == '-' || isdigit((unsigned char) *p->pos)) {
                result = parse_number(p);
            }
            break;
    }

    p->depth--;
    return result;
}

/* ========== Public Parse API ========== */

XR_FUNC XrJsonValue *xjson_parse(const char *json, size_t len) {
    if (!json)
        return NULL;

    JsonParser p = {.src = json, .end = json + len, .pos = json, .depth = 0};

    XrJsonValue *result = parse_value(&p);
    if (!result)
        return NULL;

    /* RFC 8259: no trailing content after valid value */
    skip_whitespace(&p);
    if (p.pos != p.end) {
        xjson_free(result);
        return NULL;
    }

    return result;
}

/* ========== Free ========== */

XR_FUNC void xjson_free(XrJsonValue *value) {
    if (!value)
        return;

    switch (value->type) {
        case XR_JSON_STRING:
            xr_free(value->as.string);
            break;
        case XR_JSON_ARRAY:
            for (int i = 0; i < value->as.array.count; i++) {
                xjson_free(value->as.array.items[i]);
            }
            xr_free(value->as.array.items);
            break;
        case XR_JSON_OBJECT:
            for (int i = 0; i < value->as.object.count; i++) {
                xr_free(value->as.object.members[i].key);
                xjson_free(value->as.object.members[i].value);
            }
            xr_free(value->as.object.members);
            break;
        default:
            break;
    }
    xr_free(value);
}

/* ========== Clone ========== */

XR_FUNC XrJsonValue *xjson_clone(XrJsonValue *value) {
    if (!value)
        return NULL;

    switch (value->type) {
        case XR_JSON_NULL:
            return xjson_new_null();
        case XR_JSON_BOOL:
            return xjson_new_bool(value->as.boolean);
        case XR_JSON_NUMBER: {
            XrJsonValue *n = alloc_value(XR_JSON_NUMBER);
            if (!n)
                return NULL;
            n->is_integer = value->is_integer;
            if (value->is_integer)
                n->as.integer = value->as.integer;
            else
                n->as.number = value->as.number;
            return n;
        }
        case XR_JSON_STRING:
            return xjson_new_string(value->as.string);
        case XR_JSON_ARRAY: {
            XrJsonValue *arr = xjson_new_array();
            if (!arr)
                return NULL;
            for (int i = 0; i < value->as.array.count; i++) {
                XrJsonValue *child = xjson_clone(value->as.array.items[i]);
                if (!child) {
                    xjson_free(arr);
                    return NULL;
                }
                xjson_array_push(arr, child);
            }
            return arr;
        }
        case XR_JSON_OBJECT: {
            XrJsonValue *obj = xjson_new_object();
            if (!obj)
                return NULL;
            for (int i = 0; i < value->as.object.count; i++) {
                XrJsonValue *child = xjson_clone(value->as.object.members[i].value);
                if (!child) {
                    xjson_free(obj);
                    return NULL;
                }
                xjson_object_set_new(obj, value->as.object.members[i].key, child);
            }
            return obj;
        }
    }
    return NULL;
}

/* ========== Object Accessors ========== */

XR_FUNC XrJsonValue *xjson_get(XrJsonValue *obj, const char *key) {
    if (!obj || obj->type != XR_JSON_OBJECT || !key)
        return NULL;
    for (int i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.members[i].key, key) == 0) {
            return obj->as.object.members[i].value;
        }
    }
    return NULL;
}

XR_FUNC const char *xjson_get_string(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xjson_get(obj, key);
    return (v && v->type == XR_JSON_STRING) ? v->as.string : NULL;
}

XR_FUNC int64_t xjson_get_int(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xjson_get(obj, key);
    if (!v || v->type != XR_JSON_NUMBER)
        return 0;
    return v->is_integer ? v->as.integer : (int64_t) v->as.number;
}

XR_FUNC int64_t xjson_get_int_or(XrJsonValue *obj, const char *key, int64_t default_val) {
    XrJsonValue *v = xjson_get(obj, key);
    if (!v || v->type != XR_JSON_NUMBER)
        return default_val;
    return v->is_integer ? v->as.integer : (int64_t) v->as.number;
}

XR_FUNC bool xjson_get_bool(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xjson_get(obj, key);
    return (v && v->type == XR_JSON_BOOL) ? v->as.boolean : false;
}

XR_FUNC XrJsonValue *xjson_get_array(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xjson_get(obj, key);
    return (v && v->type == XR_JSON_ARRAY) ? v : NULL;
}

XR_FUNC XrJsonValue *xjson_get_object(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xjson_get(obj, key);
    return (v && v->type == XR_JSON_OBJECT) ? v : NULL;
}

/* ========== Array Accessors ========== */

XR_FUNC int xjson_array_len(XrJsonValue *arr) {
    return (arr && arr->type == XR_JSON_ARRAY) ? arr->as.array.count : 0;
}

XR_FUNC XrJsonValue *xjson_array_get(XrJsonValue *arr, int index) {
    if (!arr || arr->type != XR_JSON_ARRAY)
        return NULL;
    if (index < 0 || index >= arr->as.array.count)
        return NULL;
    return arr->as.array.items[index];
}

/* ========== Type Checks ========== */

XR_FUNC bool xjson_is_null(XrJsonValue *v) {
    return v && v->type == XR_JSON_NULL;
}
XR_FUNC bool xjson_is_string(XrJsonValue *v) {
    return v && v->type == XR_JSON_STRING;
}
XR_FUNC bool xjson_is_number(XrJsonValue *v) {
    return v && v->type == XR_JSON_NUMBER;
}
XR_FUNC bool xjson_is_bool(XrJsonValue *v) {
    return v && v->type == XR_JSON_BOOL;
}
XR_FUNC bool xjson_is_array(XrJsonValue *v) {
    return v && v->type == XR_JSON_ARRAY;
}
XR_FUNC bool xjson_is_object(XrJsonValue *v) {
    return v && v->type == XR_JSON_OBJECT;
}

/* ========== Builder ========== */

XR_FUNC XrJsonValue *xjson_new_null(void) {
    return alloc_value(XR_JSON_NULL);
}

XR_FUNC XrJsonValue *xjson_new_bool(bool value) {
    XrJsonValue *v = alloc_value(XR_JSON_BOOL);
    if (v)
        v->as.boolean = value;
    return v;
}

XR_FUNC XrJsonValue *xjson_new_number(double value) {
    XrJsonValue *v = alloc_value(XR_JSON_NUMBER);
    if (v) {
        v->is_integer = false;
        v->as.number = value;
    }
    return v;
}

XR_FUNC XrJsonValue *xjson_new_string(const char *value) {
    XrJsonValue *v = alloc_value(XR_JSON_STRING);
    if (!v)
        return NULL;
    v->as.string = xr_strdup(value ? value : "");
    if (!v->as.string) {
        xr_free(v);
        return NULL;
    }
    return v;
}

XR_FUNC XrJsonValue *xjson_new_array(void) {
    return alloc_value(XR_JSON_ARRAY);
}

XR_FUNC XrJsonValue *xjson_new_object(void) {
    return alloc_value(XR_JSON_OBJECT);
}

XR_FUNC void xjson_array_push(XrJsonValue *arr, XrJsonValue *value) {
    if (!arr || arr->type != XR_JSON_ARRAY || !value)
        return;

    if (arr->as.array.count >= arr->as.array.capacity) {
        int new_cap = arr->as.array.capacity < 8 ? 8 : arr->as.array.capacity * 2;
        XrJsonValue **tmp = (XrJsonValue **) xr_realloc(arr->as.array.items,
                                                        (size_t) new_cap * sizeof(XrJsonValue *));
        if (!tmp)
            return;
        arr->as.array.items = tmp;
        arr->as.array.capacity = new_cap;
    }
    arr->as.array.items[arr->as.array.count++] = value;
}

XR_FUNC void xjson_array_truncate(XrJsonValue *arr, int max_len) {
    if (!arr || arr->type != XR_JSON_ARRAY)
        return;
    while (arr->as.array.count > max_len) {
        arr->as.array.count--;
        xjson_free(arr->as.array.items[arr->as.array.count]);
    }
}

XR_FUNC void xjson_object_set(XrJsonValue *obj, const char *key, XrJsonValue *value) {
    if (!obj || obj->type != XR_JSON_OBJECT || !key)
        return;

    /* Check for existing key (dedup) */
    for (int i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.members[i].key, key) == 0) {
            xjson_free(obj->as.object.members[i].value);
            obj->as.object.members[i].value = value;
            return;
        }
    }

    /* Append new member */
    if (obj->as.object.count >= obj->as.object.capacity) {
        int new_cap = obj->as.object.capacity < 8 ? 8 : obj->as.object.capacity * 2;
        XrJsonMember *tmp = (XrJsonMember *) xr_realloc(obj->as.object.members,
                                                        (size_t) new_cap * sizeof(XrJsonMember));
        if (!tmp)
            return;
        obj->as.object.members = tmp;
        obj->as.object.capacity = new_cap;
    }

    XrJsonMember *m = &obj->as.object.members[obj->as.object.count++];
    m->key = xr_strdup(key);
    m->value = value;
}

XR_FUNC void xjson_object_set_new(XrJsonValue *obj, const char *key, XrJsonValue *value) {
    if (!obj || obj->type != XR_JSON_OBJECT || !key)
        return;

    /* No dedup check — caller guarantees unique keys */
    if (obj->as.object.count >= obj->as.object.capacity) {
        int new_cap = obj->as.object.capacity < 8 ? 8 : obj->as.object.capacity * 2;
        XrJsonMember *tmp = (XrJsonMember *) xr_realloc(obj->as.object.members,
                                                        (size_t) new_cap * sizeof(XrJsonMember));
        if (!tmp)
            return;
        obj->as.object.members = tmp;
        obj->as.object.capacity = new_cap;
    }

    XrJsonMember *m = &obj->as.object.members[obj->as.object.count++];
    m->key = xr_strdup(key);
    m->value = value;
}

/* ========== Serialization ========== */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} JsonWriter;

static void writer_init(JsonWriter *w) {
    w->cap = 256;
    w->data = (char *) xr_malloc(w->cap);
    w->len = 0;
    if (w->data)
        w->data[0] = '\0';
}

static void writer_ensure(JsonWriter *w, size_t n) {
    if (!w->data)
        return;
    if (w->len + n >= w->cap) {
        size_t new_cap = w->cap * 2;
        while (new_cap < w->len + n + 1)
            new_cap *= 2;
        char *tmp = (char *) xr_realloc(w->data, new_cap);
        if (!tmp) {
            xr_free(w->data);
            w->data = NULL;
            return;
        }
        w->data = tmp;
        w->cap = new_cap;
    }
}

static void writer_append(JsonWriter *w, const char *s, size_t n) {
    writer_ensure(w, n);
    if (!w->data)
        return;
    memcpy(w->data + w->len, s, n);
    w->len += n;
    w->data[w->len] = '\0';
}

static void writer_char(JsonWriter *w, char c) {
    writer_ensure(w, 1);
    if (!w->data)
        return;
    w->data[w->len++] = c;
    w->data[w->len] = '\0';
}

static void writer_str(JsonWriter *w, const char *s) {
    writer_append(w, s, strlen(s));
}

static void stringify_value(JsonWriter *w, XrJsonValue *v);

static void stringify_string(JsonWriter *w, const char *s) {
    writer_char(w, '"');
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len) {
        /* Batch non-escape characters */
        size_t start = i;
        while (i < len) {
            unsigned char c = (unsigned char) s[i];
            if (c < 32 || c == '"' || c == '\\')
                break;
            i++;
        }
        if (i > start)
            writer_append(w, s + start, i - start);
        if (i >= len)
            break;

        unsigned char c = (unsigned char) s[i];
        switch (c) {
            case '"':
                writer_append(w, "\\\"", 2);
                break;
            case '\\':
                writer_append(w, "\\\\", 2);
                break;
            case '\n':
                writer_append(w, "\\n", 2);
                break;
            case '\r':
                writer_append(w, "\\r", 2);
                break;
            case '\t':
                writer_append(w, "\\t", 2);
                break;
            case '\b':
                writer_append(w, "\\b", 2);
                break;
            case '\f':
                writer_append(w, "\\f", 2);
                break;
            default: {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                writer_append(w, buf, 6);
                break;
            }
        }
        i++;
    }
    writer_char(w, '"');
}

static void stringify_value(JsonWriter *w, XrJsonValue *v) {
    if (!v) {
        writer_str(w, "null");
        return;
    }

    switch (v->type) {
        case XR_JSON_NULL:
            writer_str(w, "null");
            break;
        case XR_JSON_BOOL:
            writer_str(w, v->as.boolean ? "true" : "false");
            break;
        case XR_JSON_NUMBER: {
            char buf[32];
            if (v->is_integer) {
                snprintf(buf, sizeof(buf), "%lld", (long long) v->as.integer);
                writer_str(w, buf);
            } else {
                double d = v->as.number;
                if (isinf(d) || isnan(d)) {
                    writer_str(w, "null");
                } else {
                    /* Shortest round-trip: try %.15g, fallback %.17g */
                    snprintf(buf, sizeof(buf), "%.15g", d);
                    if (strtod(buf, NULL) != d) {
                        snprintf(buf, sizeof(buf), "%.17g", d);
                    }
                    writer_str(w, buf);
                }
            }
            break;
        }
        case XR_JSON_STRING:
            stringify_string(w, v->as.string ? v->as.string : "");
            break;
        case XR_JSON_ARRAY:
            writer_char(w, '[');
            for (int i = 0; i < v->as.array.count; i++) {
                if (i > 0)
                    writer_char(w, ',');
                stringify_value(w, v->as.array.items[i]);
            }
            writer_char(w, ']');
            break;
        case XR_JSON_OBJECT:
            writer_char(w, '{');
            for (int i = 0; i < v->as.object.count; i++) {
                if (i > 0)
                    writer_char(w, ',');
                stringify_string(w, v->as.object.members[i].key);
                writer_char(w, ':');
                stringify_value(w, v->as.object.members[i].value);
            }
            writer_char(w, '}');
            break;
    }
}

XR_FUNC char *xjson_stringify(XrJsonValue *value, size_t *out_len) {
    JsonWriter w;
    writer_init(&w);
    stringify_value(&w, value);

    if (out_len)
        *out_len = w.len;
    return w.data; /* caller frees via xr_free */
}
