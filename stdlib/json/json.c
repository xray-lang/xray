/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * json.c - JSON parsing and serialization implementation
 *
 * KEY CONCEPT:
 *   RFC 8259 compliant JSON parser and serializer. Handles escape sequences,
 *   Unicode (\uXXXX), and proper type conversion between JSON and xray values.
 */

#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#include "../common_writer.h"
#include "../common_parser.h"
#include "../../src/base/xmalloc.h"

/* ========== Runtime Object Headers ========== */

#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xutf8.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/base/xsimd.h"

/* ========== External Declarations ========== */

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);
extern XrValue xr_json_value(XrJson *json);

/* ========== JSON Parser ========== */

// Defer to the stdlib-wide default (256) set in common_parser.h so JSON,
// YAML, TOML and XML all refuse the same pathological input depth. The
// legacy 512 was an outlier from pre-P4 days and made the depth guard
// effectively twice as deep as every other module.
#define JSON_MAX_DEPTH XR_STDLIB_MAX_DEPTH

typedef struct {
    XrayIsolate *isolate;
    const char *json;
    const char *ptr;
    char error[256];
    int depth;
} JsonParser;

// Skip JSON whitespace (RFC 8259: space/tab/cr/lf only, no locale dependency)
static void skip_whitespace(JsonParser *p) {
    const char *s = p->ptr;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    p->ptr = s;
}

// Forward declaration
static XrValue parse_value(JsonParser *p);

// Parse 4 hex digits using lookup table, return -1 on error
static int parse_hex4(const char *s) {
    unsigned int val = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = XR_HEX_TO_VAL(s[i]);
        if (v == 255) return -1;
        val = (val << 4) | v;
    }
    return (int)val;
}

// Use xr_utf8_encode from xutf8.h (avoids duplicate implementation)

// Parse string (RFC 8259 compliant, single-pass with stack buffer)
static XrValue parse_string(JsonParser *p) {
    if (*p->ptr != '"') {
        snprintf(p->error, sizeof(p->error), "Expected '\"'");
        return xr_null();
    }
    p->ptr++;  // Skip opening quote

    // Fast path: scan for end of string to check if escapes exist
    const char *scan = p->ptr;
    while (*scan && *scan != '"' && *scan != '\\') scan++;

    if (*scan == '"' && scan > p->ptr) {
        // No escape sequences: zero-copy path
        size_t len = scan - p->ptr;
        XrString *str = xr_string_intern(p->isolate, p->ptr, len, 0);
        p->ptr = scan + 1;  // Skip closing quote
        return xr_string_value(str);
    }

    // General path: single-pass decode with stack buffer
    char stack_buf[256];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;

    #define STR_ENSURE(n) do { \
        if (len + (n) >= cap) { \
            size_t new_cap = cap * 2; \
            while (new_cap < len + (n) + 1) new_cap *= 2; \
            char *nb = (char*)xr_malloc(new_cap); \
            if (!nb) { \
                snprintf(p->error, sizeof(p->error), "Out of memory"); \
                if (buf != stack_buf) xr_free(buf); \
                return xr_null(); \
            } \
            memcpy(nb, buf, len); \
            if (buf != stack_buf) xr_free(buf); \
            buf = nb; \
            cap = new_cap; \
        } \
    } while (0)

    while (*p->ptr && *p->ptr != '"') {
        if (*p->ptr == '\\') {
            p->ptr++;
            STR_ENSURE(4);  // Max UTF-8 bytes for a single codepoint
            switch (*p->ptr) {
                case 'n': buf[len++] = '\n'; break;
                case 'r': buf[len++] = '\r'; break;
                case 't': buf[len++] = '\t'; break;
                case '\\': buf[len++] = '\\'; break;
                case '"': buf[len++] = '"'; break;
                case '/': buf[len++] = '/'; break;
                case 'b': buf[len++] = '\b'; break;
                case 'f': buf[len++] = '\f'; break;
                case 'u': {
                    p->ptr++;
                    if (!p->ptr[0] || !p->ptr[1] || !p->ptr[2] || !p->ptr[3]) {
                        snprintf(p->error, sizeof(p->error), "Incomplete unicode escape");
                        if (buf != stack_buf) xr_free(buf);
                        return xr_null();
                    }
                    int cp = parse_hex4(p->ptr);
                    if (cp < 0) {
                        snprintf(p->error, sizeof(p->error), "Invalid unicode hex digits");
                        if (buf != stack_buf) xr_free(buf);
                        return xr_null();
                    }
                    p->ptr += 3;  // Point to last digit, outer loop adds +1

                    // Handle UTF-16 surrogate pairs
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // Boundary check: need \uXXXX (6 chars) after current pos
                        if (p->ptr[1] == '\\' && p->ptr[2] == 'u' &&
                            p->ptr[3] && p->ptr[4] && p->ptr[5] && p->ptr[6]) {
                            int low = parse_hex4(p->ptr + 3);
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                unsigned int full_cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                len += xr_utf8_encode(full_cp, buf + len);
                                p->ptr += 6;
                            } else {
                                snprintf(p->error, sizeof(p->error), "Invalid surrogate pair");
                                if (buf != stack_buf) xr_free(buf);
                                return xr_null();
                            }
                        } else {
                            snprintf(p->error, sizeof(p->error), "Lone high surrogate");
                            if (buf != stack_buf) xr_free(buf);
                            return xr_null();
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        snprintf(p->error, sizeof(p->error), "Lone low surrogate");
                        if (buf != stack_buf) xr_free(buf);
                        return xr_null();
                    } else {
                        len += xr_utf8_encode((uint32_t)cp, buf + len);
                    }
                    break;
                }
                default:
                    snprintf(p->error, sizeof(p->error), "Invalid escape sequence '\\%c'", *p->ptr);
                    if (buf != stack_buf) xr_free(buf);
                    return xr_null();
            }
        } else {
            STR_ENSURE(1);
            buf[len++] = *p->ptr;
        }
        p->ptr++;
    }

    #undef STR_ENSURE

    if (*p->ptr != '"') {
        snprintf(p->error, sizeof(p->error), "Unclosed string");
        if (buf != stack_buf) xr_free(buf);
        return xr_null();
    }
    p->ptr++;  // Skip closing quote

    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf) xr_free(buf);
    return xr_string_value(str);
}

// Parse number
static XrValue parse_number(JsonParser *p) {
    const char *start = p->ptr;
    bool is_float = false;

    if (*p->ptr == '-') p->ptr++;  // Negative sign

    // RFC 8259: leading zeros not allowed (except "0" itself)
    const char *digit_start = p->ptr;
    while (isdigit((unsigned char)*p->ptr)) p->ptr++;  // Integer part
    int digit_count = (int)(p->ptr - digit_start);
    if (digit_count > 1 && *digit_start == '0') {
        snprintf(p->error, sizeof(p->error), "Leading zeros not allowed");
        return xr_null();
    }

    // Fractional part (RFC 8259: at least one digit after '.')
    if (*p->ptr == '.') {
        is_float = true;
        p->ptr++;
        if (!isdigit((unsigned char)*p->ptr)) {
            snprintf(p->error, sizeof(p->error), "Invalid number: no digits after decimal point");
            return xr_null();
        }
        while (isdigit((unsigned char)*p->ptr)) p->ptr++;
    }

    // Exponent part (RFC 8259: at least one digit after 'e'/'E')
    if (*p->ptr == 'e' || *p->ptr == 'E') {
        is_float = true;
        p->ptr++;
        if (*p->ptr == '+' || *p->ptr == '-') p->ptr++;
        if (!isdigit((unsigned char)*p->ptr)) {
            snprintf(p->error, sizeof(p->error), "Invalid number: no digits in exponent");
            return xr_null();
        }
        while (isdigit((unsigned char)*p->ptr)) p->ptr++;
    }

    char *end;
    if (is_float) {
        double val = strtod(start, &end);
        return xr_float(val);
    } else {
        // Tagged Union: full 64-bit int, fall back to float only on overflow
        errno = 0;
        int64_t val = strtoll(start, &end, 10);
        if (errno == ERANGE) {
            double fval = strtod(start, &end);
            return xr_float(fval);
        }
        return xr_int(val);
    }
}

// Parse array
static XrValue parse_array(JsonParser *p) {
    if (*p->ptr != '[') {
        snprintf(p->error, sizeof(p->error), "Expected '['");
        return xr_null();
    }
    p->ptr++;  // Skip [

    XrArray *arr = xr_array_new(xr_current_coro(p->isolate));

    skip_whitespace(p);
    if (*p->ptr == ']') {
        p->ptr++;
        return xr_value_from_array(arr);
    }

    while (1) {
        skip_whitespace(p);
        XrValue elem = parse_value(p);
        if (p->error[0]) return xr_null();

        xr_array_push(arr, elem);

        skip_whitespace(p);
        if (*p->ptr == ']') {
            p->ptr++;
            break;
        }
        if (*p->ptr != ',') {
            snprintf(p->error, sizeof(p->error), "Expected ',' or ']'");
            return xr_null();
        }
        p->ptr++;
    }

    return xr_value_from_array(arr);
}

// Quick scan to count top-level keys in a JSON object (for pre-allocation).
// Counts ':' at depth 0, skipping nested objects/arrays/strings.
static uint16_t count_object_keys(const char *start) {
    const char *s = start;
    uint16_t count = 0;
    int depth = 0;
    bool in_string = false;
    while (*s && !(depth == 0 && *s == '}')) {
        if (in_string) {
            if (*s == '\\') { s++; if (*s) s++; continue; }
            if (*s == '"') in_string = false;
            s++;
            continue;
        }
        if (*s == '"') { in_string = true; s++; continue; }
        if (*s == '{' || *s == '[') { depth++; s++; continue; }
        if (*s == '}' || *s == ']') { depth--; s++; continue; }
        if (*s == ':' && depth == 0) count++;
        s++;
    }
    return count < 4 ? 4 : count;
}

// Parse object - returns XrJson type
static XrValue parse_object(JsonParser *p) {
    if (*p->ptr != '{') {
        snprintf(p->error, sizeof(p->error), "Expected '{'");
        return xr_null();
    }
    p->ptr++;  // Skip {

    // Pre-scan to determine exact capacity needed
    uint16_t capacity = count_object_keys(p->ptr);
    XrJson *json = xr_json_new(xr_current_coro(p->isolate), capacity);
    if (!json) {
        snprintf(p->error, sizeof(p->error), "Failed to create Json object");
        return xr_null();
    }

    skip_whitespace(p);
    if (*p->ptr == '}') {
        p->ptr++;
        return xr_json_value(json);
    }

    while (1) {
        skip_whitespace(p);

        // Parse key
        if (*p->ptr != '"') {
            snprintf(p->error, sizeof(p->error), "Object key must be a string");
            return xr_null();
        }
        XrValue key = parse_string(p);
        if (p->error[0]) return xr_null();

        skip_whitespace(p);
        if (*p->ptr != ':') {
            snprintf(p->error, sizeof(p->error), "Expected ':'");
            return xr_null();
        }
        p->ptr++;

        // Parse value
        skip_whitespace(p);
        XrValue val = parse_value(p);
        if (p->error[0]) return xr_null();

        // Set field using string key
        XrString *key_str = XR_TO_STRING(key);
        xr_json_set_by_key(p->isolate, json, key_str->data, val);

        skip_whitespace(p);
        if (*p->ptr == '}') {
            p->ptr++;
            break;
        }
        if (*p->ptr != ',') {
            snprintf(p->error, sizeof(p->error), "Expected ',' or '}'");
            return xr_null();
        }
        p->ptr++;
    }

    return xr_json_value(json);
}

// Parse value
static XrValue parse_value(JsonParser *p) {
    skip_whitespace(p);

    if (p->depth >= JSON_MAX_DEPTH) {
        snprintf(p->error, sizeof(p->error), "Maximum nesting depth exceeded");
        return xr_null();
    }
    p->depth++;

    XrValue result;
    switch (*p->ptr) {
        case '"': result = parse_string(p); p->depth--; return result;
        case '[': result = parse_array(p); p->depth--; return result;
        case '{': result = parse_object(p); p->depth--; return result;
        case 't': // true
            if (strncmp(p->ptr, "true", 4) == 0) {
                p->ptr += 4;
                p->depth--;
                return xr_bool(true);
            }
            break;
        case 'f': // false
            if (strncmp(p->ptr, "false", 5) == 0) {
                p->ptr += 5;
                p->depth--;
                return xr_bool(false);
            }
            break;
        case 'n': // null
            if (strncmp(p->ptr, "null", 4) == 0) {
                p->ptr += 4;
                p->depth--;
                return xr_null();
            }
            break;
        default:
            if (*p->ptr == '-' || isdigit((unsigned char)*p->ptr)) {
                result = parse_number(p);
                p->depth--;
                return result;
            }
            break;
    }

    snprintf(p->error, sizeof(p->error), "Invalid JSON value");
    p->depth--;
    return xr_null();
}

/* ========== JSON Serialization ========== */

typedef struct {
    XrSerWriter sw;    // shared buffer: sw.data / sw.len / sw.cap
    XrayIsolate *isolate;
    int indent;
    int depth;
} JsonWriter;

static inline void writer_init(JsonWriter *w, XrayIsolate *isolate, int indent) {
    xr_serw_init(&w->sw, 1024);
    w->isolate = isolate;
    w->indent = indent;
    w->depth = 0;
}

static inline void writer_free(JsonWriter *w) {
    xr_serw_free(&w->sw);
}

static inline void writer_append(JsonWriter *w, const char *s, size_t n) {
    xr_serw_append(&w->sw, s, n);
}

static inline void writer_char(JsonWriter *w, char c) {
    xr_serw_char(&w->sw, c);
}

static inline void writer_str(JsonWriter *w, const char *s) {
    xr_serw_str(&w->sw, s);
}

static inline void writer_newline(JsonWriter *w) {
    xr_serw_newline(&w->sw, w->depth, w->indent);
}

// Forward declaration
static void stringify_value(JsonWriter *w, XrValue val);

// Stringify string: batch non-escape spans for fewer writer calls
static void stringify_string(JsonWriter *w, const char *s, size_t len) {
    writer_char(w, '"');
    size_t i = 0;
    while (i < len) {
        // Scan ahead for characters that don't need escaping
        size_t start = i;
        while (i < len) {
            unsigned char c = s[i];
            if (c < 32 || c == '"' || c == '\\') break;
            i++;
        }
        // Flush non-escape span in one call
        if (i > start) {
            writer_append(w, s + start, i - start);
        }
        if (i >= len) break;
        // Handle escape character
        unsigned char c = s[i];
        switch (c) {
            case '"': writer_append(w, "\\\"", 2); break;
            case '\\': writer_append(w, "\\\\", 2); break;
            case '\n': writer_append(w, "\\n", 2); break;
            case '\r': writer_append(w, "\\r", 2); break;
            case '\t': writer_append(w, "\\t", 2); break;
            case '\b': writer_append(w, "\\b", 2); break;
            case '\f': writer_append(w, "\\f", 2); break;
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

// Stringify array
static void stringify_array(JsonWriter *w, XrArray *arr) {
    writer_char(w, '[');

    int count = arr->length;
    if (count > 0) {
        w->depth++;
        for (int i = 0; i < count; i++) {
            if (i > 0) writer_char(w, ',');
            writer_newline(w);
            stringify_value(w, xr_array_get(arr, i));
        }
        w->depth--;
        writer_newline(w);
    }

    writer_char(w, ']');
}

// Stringify Map
static void stringify_map(JsonWriter *w, XrMap *map) {
    writer_char(w, '{');

    // Iterate over Map hash table
    size_t output_count = 0;
    w->depth++;

    // Get node count
    uint32_t size = (map->flags & XR_MAP_FLAG_DUMMY) ? 0 : xr_map_sizenode(map);

    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(map, i);

        // key_tt > 0 indicates valid key
        if (node->key_tt > 0) {
            if (output_count > 0) writer_char(w, ',');
            writer_newline(w);

            // Key must be string
            if (XR_IS_STRING(node->key)) {
                XrString *key = XR_TO_STRING(node->key);
                stringify_string(w, key->data, key->length);
            } else {
                // Non-string key fallback
                writer_str(w, "\"<key>\"");
            }

            writer_char(w, ':');
            if (w->indent > 0) writer_char(w, ' ');

            stringify_value(w, node->value);
            output_count++;
        }
    }

    w->depth--;
    if (output_count > 0) writer_newline(w);
    writer_char(w, '}');
}

// Stringify XrJson object
static void stringify_json(JsonWriter *w, XrJson *json) {
    writer_char(w, '{');

    if (!json || !xr_json_shape(json)) {
        writer_char(w, '}');
        return;
    }

    w->depth++;
    size_t output_count = 0;

    XrShape *shape = xr_json_shape(json);
    XrSymbolTable *symtab = (XrSymbolTable*)w->isolate->symbol_table;

    for (uint16_t i = 0; i < shape->field_count; i++) {
        if (output_count > 0) writer_char(w, ',');
        writer_newline(w);

        // Get field name
        SymbolId sym = shape->field_symbols[i];
        const char *name = xr_symbol_get_name_in_table(symtab, sym);
        if (name) {
            stringify_string(w, name, strlen(name));
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "field%d", i);
            stringify_string(w, buf, strlen(buf));
        }

        writer_char(w, ':');
        if (w->indent > 0) writer_char(w, ' ');
        stringify_value(w, xr_json_get_field_any(json, i));
        output_count++;
    }

    w->depth--;
    if (output_count > 0) writer_newline(w);
    writer_char(w, '}');
}

// Stringify Instance (Struct/Class instance)
static void stringify_instance(JsonWriter *w, XrInstance *inst) {
    writer_char(w, '{');

    XrClass *cls = xr_instance_get_class(inst);
    if (!cls) {
        writer_char(w, '}');
        return;
    }

    w->depth++;
    size_t output_count = 0;

    // Iterate over all fields (including inherited)
    for (uint16_t i = 0; i < cls->field_count; i++) {
        // Skip static fields
        if (cls->fields[i].flags & XR_FIELD_STATIC) continue;

        // Skip private fields (optional: can include them)
        // if (cls->fields[i].flags & XR_FIELD_PRIVATE) continue;

        if (output_count > 0) writer_char(w, ',');
        writer_newline(w);

        // Field name
        const char *name = cls->fields[i].name;
        if (name) {
            stringify_string(w, name, strlen(name));
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "field%d", i);
            stringify_string(w, buf, strlen(buf));
        }

        writer_char(w, ':');
        if (w->indent > 0) writer_char(w, ' ');

        // Field value (access by index)
        XrValue field_val = xr_instance_get_field_fast(inst, i);
        stringify_value(w, field_val);
        output_count++;
    }

    w->depth--;
    if (output_count > 0) writer_newline(w);
    writer_char(w, '}');
}

// Stringify value (with depth guard against circular/deep nesting)
static void stringify_value(JsonWriter *w, XrValue val) {
    if (w->depth >= JSON_MAX_DEPTH) {
        writer_str(w, "null");
        return;
    }
    if (XR_IS_NULL(val)) {
        writer_str(w, "null");
    } else if (XR_IS_BOOL(val)) {
        writer_str(w, XR_TO_BOOL(val) ? "true" : "false");
    } else if (XR_IS_INT(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)XR_TO_INT(val));
        writer_str(w, buf);
    } else if (XR_IS_FLOAT(val)) {
        double d = XR_TO_FLOAT(val);
        char buf[32];
        if (isinf(d) || isnan(d)) {
            writer_str(w, "null");  // JSON doesn't support Infinity/NaN
        } else {
            snprintf(buf, sizeof(buf), "%.17g", d);
            writer_str(w, buf);
        }
    } else if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        stringify_string(w, s->data, s->length);
    } else if (XR_IS_ARRAY(val)) {
        stringify_array(w, XR_TO_ARRAY(val));
    } else if (xr_value_is_json(val)) {
        stringify_json(w, xr_value_to_json(val));
    } else if (XR_IS_MAP(val)) {
        stringify_map(w, XR_TO_MAP(val));
    } else if (xr_value_is_instance(val)) {
        // Struct or Class instance
        stringify_instance(w, (XrInstance*)XR_TO_PTR(val));
    } else {
        // Unsupported type
        writer_str(w, "null");
    }
}

/* ========== Module Functions ========== */

// parse(str) - Parse JSON string
static XrValue json_parse(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }

    XrString *str = XR_TO_STRING(args[0]);

    JsonParser parser = {
        .isolate = X,
        .json = str->data,
        .ptr = str->data,
        .error = {0},
        .depth = 0
    };

    XrValue result = parse_value(&parser);

    if (parser.error[0]) {
        return xr_null();
    }

    // B1: Check for trailing content after valid JSON value
    skip_whitespace(&parser);
    if (*parser.ptr != '\0') {
        return xr_null();
    }

    return result;
}

// stringify(value, indent?) - Serialize to JSON string
static XrValue json_stringify(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) return xr_null();

    int indent = 0;
    if (argc >= 2 && XR_IS_INT(args[1])) {
        indent = (int)XR_TO_INT(args[1]);
        if (indent < 0) indent = 0;
        if (indent > 8) indent = 8;
    }

    JsonWriter writer;
    writer_init(&writer, X, indent);

    stringify_value(&writer, args[0]);

    XrString *str = xr_string_new(X, writer.sw.data, writer.sw.len);
    writer_free(&writer);

    return xr_string_value(str);
}

// Public API: serialize XrValue to JSON C-string.
// Caller MUST release the returned pointer with xr_free() (not free()) so
// that the debug allocator tracks the deallocation correctly.
char* xr_json_stringify_to_cstr(XrayIsolate *X, XrValue val, size_t *out_len) {
    JsonWriter writer;
    writer_init(&writer, X, 0);
    stringify_value(&writer, val);

    // Null-terminate
    writer_char(&writer, '\0');
    if (out_len) *out_len = writer.sw.len - 1;  // exclude null terminator
    return xr_serw_steal(&writer.sw);
}

// Public API: parse JSON C-string to XrValue
// Returns xr_null() on parse error. Input need not be null-terminated if len is provided.
XrValue xr_json_parse_from_cstr(XrayIsolate *X, const char *json_str, size_t len) {
    if (!X || !json_str || len == 0) return xr_null();

    // Always make a null-terminated copy (avoids out-of-bounds read on json_str[len])
    char *buf = (char *)xr_malloc(len + 1);
    if (!buf) return xr_null();
    memcpy(buf, json_str, len);
    buf[len] = '\0';
    const char *src = buf;

    JsonParser parser = {
        .isolate = X,
        .json = src,
        .ptr = src,
        .error = {0},
        .depth = 0
    };

    XrValue result = parse_value(&parser);

    if (parser.error[0]) {
        xr_free(buf);
        return xr_null();
    }

    // Check for trailing content
    skip_whitespace(&parser);
    if (*parser.ptr != '\0') {
        xr_free(buf);
        return xr_null();
    }

    xr_free(buf);
    return result;
}

/* ========== Lightweight JSON Validator (zero GC allocation) ========== */

typedef struct {
    const char *ptr;
    int depth;
    bool ok;
} JsonValidator;

static void validate_skip_ws(JsonValidator *v) {
    while (*v->ptr && isspace((unsigned char)*v->ptr)) v->ptr++;
}

static void validate_value(JsonValidator *v);

static void validate_string(JsonValidator *v) {
    if (*v->ptr != '"') { v->ok = false; return; }
    v->ptr++;
    while (*v->ptr && *v->ptr != '"') {
        if (*v->ptr == '\\') {
            v->ptr++;
            if (!*v->ptr) { v->ok = false; return; }
            if (*v->ptr == 'u') {
                for (int i = 0; i < 4; i++) {
                    v->ptr++;
                    if (!isxdigit((unsigned char)*v->ptr)) { v->ok = false; return; }
                }
            } else {
                const char *valid = "\"\\/bfnrt";
                if (!strchr(valid, *v->ptr)) { v->ok = false; return; }
            }
        }
        v->ptr++;
    }
    if (*v->ptr != '"') { v->ok = false; return; }
    v->ptr++;
}

static void validate_number(JsonValidator *v) {
    if (*v->ptr == '-') v->ptr++;
    if (!isdigit((unsigned char)*v->ptr)) { v->ok = false; return; }
    const char *dstart = v->ptr;
    while (isdigit((unsigned char)*v->ptr)) v->ptr++;
    if ((v->ptr - dstart) > 1 && *dstart == '0') { v->ok = false; return; }
    if (*v->ptr == '.') {
        v->ptr++;
        if (!isdigit((unsigned char)*v->ptr)) { v->ok = false; return; }
        while (isdigit((unsigned char)*v->ptr)) v->ptr++;
    }
    if (*v->ptr == 'e' || *v->ptr == 'E') {
        v->ptr++;
        if (*v->ptr == '+' || *v->ptr == '-') v->ptr++;
        if (!isdigit((unsigned char)*v->ptr)) { v->ok = false; return; }
        while (isdigit((unsigned char)*v->ptr)) v->ptr++;
    }
}

static void validate_array(JsonValidator *v) {
    v->ptr++;
    validate_skip_ws(v);
    if (*v->ptr == ']') { v->ptr++; return; }
    while (v->ok) {
        validate_skip_ws(v);
        validate_value(v);
        if (!v->ok) return;
        validate_skip_ws(v);
        if (*v->ptr == ']') { v->ptr++; return; }
        if (*v->ptr != ',') { v->ok = false; return; }
        v->ptr++;
    }
}

static void validate_object(JsonValidator *v) {
    v->ptr++;
    validate_skip_ws(v);
    if (*v->ptr == '}') { v->ptr++; return; }
    while (v->ok) {
        validate_skip_ws(v);
        validate_string(v);
        if (!v->ok) return;
        validate_skip_ws(v);
        if (*v->ptr != ':') { v->ok = false; return; }
        v->ptr++;
        validate_skip_ws(v);
        validate_value(v);
        if (!v->ok) return;
        validate_skip_ws(v);
        if (*v->ptr == '}') { v->ptr++; return; }
        if (*v->ptr != ',') { v->ok = false; return; }
        v->ptr++;
    }
}

static void validate_value(JsonValidator *v) {
    if (!v->ok) return;
    if (v->depth >= JSON_MAX_DEPTH) { v->ok = false; return; }
    v->depth++;
    validate_skip_ws(v);
    switch (*v->ptr) {
        case '"': validate_string(v); break;
        case '[': validate_array(v); break;
        case '{': validate_object(v); break;
        case 't':
            if (strncmp(v->ptr, "true", 4) == 0) { v->ptr += 4; }
            else { v->ok = false; }
            break;
        case 'f':
            if (strncmp(v->ptr, "false", 5) == 0) { v->ptr += 5; }
            else { v->ok = false; }
            break;
        case 'n':
            if (strncmp(v->ptr, "null", 4) == 0) { v->ptr += 4; }
            else { v->ok = false; }
            break;
        default:
            if (*v->ptr == '-' || isdigit((unsigned char)*v->ptr)) {
                validate_number(v);
            } else {
                v->ok = false;
            }
            break;
    }
    v->depth--;
}

// isValid(str) - Check if string is valid JSON (zero allocation)
static XrValue json_is_valid(XrayIsolate *X, XrValue *args, int argc) {
    (void)X;
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_bool(false);
    }

    XrString *str = XR_TO_STRING(args[0]);
    JsonValidator v = { .ptr = str->data, .depth = 0, .ok = true };

    validate_value(&v);
    if (!v.ok) return xr_bool(false);

    validate_skip_ws(&v);
    if (*v.ptr != '\0') return xr_bool(false);

    return xr_bool(true);
}

// typeOf(value) - Get JSON type name from a JSON string
// Returns: "null", "boolean", "number", "string", "array", "object", "invalid"
static XrValue json_type_of(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_string_value(xr_string_intern(X, "undefined", 9, 0));
    }

    XrValue val = args[0];
    const char *type;

    // If argument is a string, parse it as JSON and return the type
    if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        const char *s = str->data;
        // Skip whitespace
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

        if (*s == 'n' && strncmp(s, "null", 4) == 0) type = "null";
        else if (*s == 't' && strncmp(s, "true", 4) == 0) type = "boolean";
        else if (*s == 'f' && strncmp(s, "false", 5) == 0) type = "boolean";
        else if (*s == '"') type = "string";
        else if (*s == '[') type = "array";
        else if (*s == '{') type = "object";
        else if (*s == '-' || (*s >= '0' && *s <= '9')) type = "number";
        else type = "invalid";
    } else if (XR_IS_NULL(val)) {
        type = "null";
    } else if (XR_IS_BOOL(val)) {
        type = "boolean";
    } else if (XR_IS_INT(val) || XR_IS_FLOAT(val)) {
        type = "number";
    } else if (XR_IS_ARRAY(val)) {
        type = "array";
    } else if (xr_value_is_json(val) || XR_IS_MAP(val) || xr_value_is_instance(val)) {
        type = "object";
    } else {
        type = "unknown";
    }

    return xr_string_value(xr_string_intern(X, type, strlen(type), 0));
}

// tryParse(str) - Try to parse JSON
// Returns Json: {value: parsed result, error: error message or null}
static XrValue json_try_parse(XrayIsolate *X, XrValue *args, int argc) {
    XrJson *result = xr_json_new(xr_current_coro(X), 4);

    if (argc < 1 || !XR_IS_STRING(args[0])) {
        xr_json_set_by_key(X, result, "error",
            xr_string_value(xr_string_intern(X, "Argument must be a string", 25, 0)));
        xr_json_set_by_key(X, result, "value", xr_null());
        return xr_json_value(result);
    }

    XrString *str = XR_TO_STRING(args[0]);
    JsonParser parser = {
        .isolate = X,
        .json = str->data,
        .ptr = str->data,
        .error = {0},
        .depth = 0
    };

    XrValue parsed = parse_value(&parser);

    if (parser.error[0]) {
        xr_json_set_by_key(X, result, "value", xr_null());
        xr_json_set_by_key(X, result, "error",
            xr_string_value(xr_string_intern(X, parser.error, strlen(parser.error), 0)));
    } else {
        // B5: Check for trailing content
        skip_whitespace(&parser);
        if (*parser.ptr != '\0') {
            const char *msg = "Unexpected content after JSON value";
            xr_json_set_by_key(X, result, "value", xr_null());
            xr_json_set_by_key(X, result, "error",
                xr_string_value(xr_string_intern(X, msg, strlen(msg), 0)));
        } else {
            xr_json_set_by_key(X, result, "value", parsed);
            xr_json_set_by_key(X, result, "error", xr_null());
        }
    }

    return xr_json_value(result);
}

// keys(obj) - Get all keys of object
static XrValue json_keys(XrayIsolate *X, XrValue *args, int argc) {
    XrArray *keys = xr_array_new(xr_current_coro(X));

    if (argc < 1) return xr_value_from_array(keys);

    XrValue val = args[0];
    XrSymbolTable *symtab = (XrSymbolTable*)X->symbol_table;

    if (xr_value_is_json(val)) {
        // XrJson object
        XrJson *json = xr_value_to_json(val);
        if (json && xr_json_shape(json)) {
            XrShape *shape = xr_json_shape(json);
            for (uint16_t i = 0; i < shape->field_count; i++) {
                SymbolId sym = shape->field_symbols[i];
                const char *name = xr_symbol_get_name_in_table(symtab, sym);
                if (name) {
                    xr_array_push(keys, xr_string_value(xr_string_intern(X, name, strlen(name), 0)));
                }
            }
        }
    } else if (XR_IS_MAP(val)) {
        XrMap *map = XR_TO_MAP(val);
        uint32_t size = (map->flags & XR_MAP_FLAG_DUMMY) ? 0 : xr_map_sizenode(map);

        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *node = xr_map_node(map, i);
            if (node->key_tt > 0) {
                xr_array_push(keys, node->key);
            }
        }
    } else if (xr_value_is_instance(val)) {
        XrInstance *inst = (XrInstance*)XR_TO_PTR(val);
        XrClass *cls = xr_instance_get_class(inst);
        if (cls) {
            for (uint16_t i = 0; i < cls->field_count; i++) {
                if (!(cls->fields[i].flags & XR_FIELD_STATIC)) {
                    const char *name = cls->fields[i].name;
                    if (name) {
                        XrValue key = xr_string_value(xr_string_intern(X, name, strlen(name), 0));
                        xr_array_push(keys, key);
                    }
                }
            }
        }
    }

    return xr_value_from_array(keys);
}

// values(obj) - Get all values of object
static XrValue json_values(XrayIsolate *X, XrValue *args, int argc) {
    XrArray *values = xr_array_new(xr_current_coro(X));

    if (argc < 1) return xr_value_from_array(values);

    XrValue val = args[0];

    if (xr_value_is_json(val)) {
        // XrJson object
        XrJson *json = xr_value_to_json(val);
        if (json && xr_json_shape(json)) {
            XrShape *shape = xr_json_shape(json);
            for (uint16_t i = 0; i < shape->field_count; i++) {
                xr_array_push(values, xr_json_get_field_any(json, i));
            }
        }
    } else if (XR_IS_MAP(val)) {
        XrMap *map = XR_TO_MAP(val);
        uint32_t size = (map->flags & XR_MAP_FLAG_DUMMY) ? 0 : xr_map_sizenode(map);

        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *node = xr_map_node(map, i);
            if (node->key_tt > 0) {
                xr_array_push(values, node->value);
            }
        }
    } else if (xr_value_is_instance(val)) {
        XrInstance *inst = (XrInstance*)XR_TO_PTR(val);
        XrClass *cls = xr_instance_get_class(inst);
        if (cls) {
            for (uint16_t i = 0; i < cls->field_count; i++) {
                if (!(cls->fields[i].flags & XR_FIELD_STATIC)) {
                    XrValue field_val = xr_instance_get_field_fast(inst, i);
                    xr_array_push(values, field_val);
                }
            }
        }
    }

    return xr_value_from_array(values);
}

/* ========== Type Declarations (parsed by gen_stdlib_types.py) ========== */

#include "../../src/module/xbuiltin_decl.h"

// @module json

XR_DEFINE_BUILTIN(json_parse, "parse", "(s: string): any", "Parse JSON string")
XR_DEFINE_BUILTIN(json_stringify, "stringify", "(value: any, indent?: int): string", "Convert value to JSON string")
XR_DEFINE_BUILTIN(json_is_valid, "isValid", "(s: string): bool", "Check if string is valid JSON")
XR_DEFINE_BUILTIN(json_type_of, "typeof", "(value: any): string", "Get JSON type name")
XR_DEFINE_BUILTIN(json_try_parse, "tryParse", "(s: string): Json", "Safe parse, returns {value, error}")
XR_DEFINE_BUILTIN(json_keys, "keys", "(obj: Json): Array<string>", "Get object keys")
XR_DEFINE_BUILTIN(json_values, "values", "(obj: Json): Array<any>", "Get object values")

/* ========== Module Loading ========== */

XrModule* xr_load_module_json(XrayIsolate *isolate) {
    // Create native module
    XrModule *mod = xr_module_create_native(isolate, "json");
    if (!mod) return NULL;

    // Add exported functions
    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);

    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)

    EXPORT_CFUNC("parse", json_parse);
    EXPORT_CFUNC("stringify", json_stringify);
    EXPORT_CFUNC("isValid", json_is_valid);
    EXPORT_CFUNC("typeof", json_type_of);
    EXPORT_CFUNC("tryParse", json_try_parse);
    EXPORT_CFUNC("keys", json_keys);
    EXPORT_CFUNC("values", json_values);

    #undef EXPORT_CFUNC

    // Mark as loaded
    mod->loaded = true;
    return mod;
}
