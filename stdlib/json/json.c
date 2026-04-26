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
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../common_writer.h"
#include "../common_parser.h"
#include "../../src/base/xmalloc.h"

/* ========== Runtime Object Headers ========== */

#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/base/xsimd.h"
#include "../../src/base/xjson.h"

/* ========== External Declarations ========== */

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);
extern XrValue xr_json_value(XrJson *json);

/* ========== JSON Parser (delegates to src/base/xjson) ========== */

// Depth limit shared with validator (defined in common_parser.h)
#define JSON_MAX_DEPTH XR_STDLIB_MAX_DEPTH

/*
 * Convert a pure-C XrJsonValue DOM tree (from src/base/xjson) to runtime
 * XrValue objects (XrString, XrArray, XrJson). This is the bridge layer
 * that avoids duplicating parser code in the stdlib.
 *
 * Number heuristic: JSON has no integer type; we return xr_int when the
 * double has no fractional part and fits in int64, else xr_float.
 */
static XrValue dom_to_xrvalue(XrayIsolate *X, XrJsonValue *v) {
    if (!v) return xr_null();

    switch (v->type) {
        case XR_JSON_NULL:
            return xr_null();

        case XR_JSON_BOOL:
            return xr_bool(v->as.boolean);

        case XR_JSON_NUMBER:
            if (v->is_integer) {
                return xr_int(v->as.integer);
            }
            return xr_float(v->as.number);

        case XR_JSON_STRING: {
            size_t len = strlen(v->as.string);
            XrString *str = xr_string_intern(X, v->as.string, len, 0);
            return xr_string_value(str);
        }

        case XR_JSON_ARRAY: {
            XrArray *arr = xr_array_new(xr_current_coro(X));
            for (int idx = 0; idx < v->as.array.count; idx++) {
                xr_array_push(arr, dom_to_xrvalue(X, v->as.array.items[idx]));
            }
            return xr_value_from_array(arr);
        }

        case XR_JSON_OBJECT: {
            uint16_t cap = v->as.object.count > 4
                         ? (uint16_t)v->as.object.count
                         : 4;
            XrJson *json = xr_json_new(xr_current_coro(X), cap);
            if (!json) return xr_null();
            for (int idx = 0; idx < v->as.object.count; idx++) {
                xr_json_set_by_key(X, json,
                    v->as.object.members[idx].key,
                    dom_to_xrvalue(X, v->as.object.members[idx].value));
            }
            return xr_json_value(json);
        }
    }
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

// Stringify Map.
//
// NOTE: ordering. XrMap is a chained hash table (xmap.h) and does not
// track insertion order. We iterate node slots in index order, which
// is deterministic for a given map instance but unrelated to the order
// entries were inserted. Scripts that need insertion-preserving
// round-trip should build their object via XrJson (shape-backed, which
// `stringify_json` preserves exactly).
static void stringify_map(JsonWriter *w, XrMap *map) {
    writer_char(w, '{');

    size_t output_count = 0;
    w->depth++;

    // Get node count
    uint32_t size = (map->flags & XR_MAP_FLAG_DUMMY) ? 0 : xr_map_sizenode(map);

    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (node->key_tt == 0) continue;

        // Keys must be JSON-representable. Strings are emitted
        // verbatim; integers are stringified (acceptable per common
        // JSON-as-config usage). Any other key type (float, array,
        // map, instance) is skipped — previously we emitted the
        // placeholder `"<key>"`, which silently collides on read-back
        // and is worse than losing the entry.
        char intkey_buf[32];
        const char *key_ptr = NULL;
        size_t key_len = 0;
        if (XR_IS_STRING(node->key)) {
            XrString *key = XR_TO_STRING(node->key);
            key_ptr = key->data;
            key_len = key->length;
        } else if (XR_IS_INT(node->key)) {
            int n = snprintf(intkey_buf, sizeof(intkey_buf),
                             "%lld", (long long)XR_TO_INT(node->key));
            if (n > 0) {
                key_ptr = intkey_buf;
                key_len = (size_t)n;
            }
        } else {
            continue;  // skip entries with non-stringifiable keys
        }

        if (output_count > 0) writer_char(w, ',');
        writer_newline(w);

        stringify_string(w, key_ptr, key_len);
        writer_char(w, ':');
        if (w->indent > 0) writer_char(w, ' ');

        stringify_value(w, node->value);
        output_count++;
    }

    w->depth--;
    if (output_count > 0) writer_newline(w);
    writer_char(w, '}');
}

// Stringify XrJson object
static void stringify_json(JsonWriter *w, XrJson *json) {
    writer_char(w, '{');

    if (!json || !xr_json_shape(w->isolate, json)) {
        writer_char(w, '}');
        return;
    }

    w->depth++;
    size_t output_count = 0;

    XrShape *shape = xr_json_shape(w->isolate, json);
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
        stringify_value(w, xr_json_get_field_any(w->isolate, json, i));
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
            // Shortest round-trip: try %.15g first (DBL_DIG); only
            // fall back to %.17g if the shorter form doesn't round-trip.
            snprintf(buf, sizeof(buf), "%.15g", d);
            if (strtod(buf, NULL) != d)
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
    XrJsonValue *dom = xjson_parse(str->data, str->length);
    if (!dom) return xr_null();

    XrValue result = dom_to_xrvalue(X, dom);
    xjson_free(dom);
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

    XrJsonValue *dom = xjson_parse(json_str, len);
    if (!dom) return xr_null();

    XrValue result = dom_to_xrvalue(X, dom);
    xjson_free(dom);
    return result;
}

/* ========== Lightweight JSON Validator (zero GC allocation) ========== */

typedef struct {
    const char *ptr;
    int depth;
    bool ok;
    bool strict;   // RFC 8259 strict mode: reject bare control chars (< 0x20)
} JsonValidator;

static void validate_skip_ws(JsonValidator *v) {
    while (*v->ptr && isspace((unsigned char)*v->ptr)) v->ptr++;
}

static void validate_value(JsonValidator *v);

static void validate_string(JsonValidator *v) {
    if (*v->ptr != '"') { v->ok = false; return; }
    v->ptr++;
    while (*v->ptr && *v->ptr != '"') {
        unsigned char c = (unsigned char)*v->ptr;
        // RFC 8259 §7: unescaped bytes <= 0x1F inside a string are
        // forbidden. The previous validator silently accepted them so
        // `isValid("\"\\x01\"")` returned true. Under the new strict
        // flag we flag those while keeping the legacy non-strict path
        // permissive for existing callers.
        if (v->strict && c < 0x20) { v->ok = false; return; }
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

// isValid(str, opts?) - Check if string is valid JSON (zero allocation).
// opts.strict (bool, default false): when true, additionally reject
// unescaped control bytes (< 0x20) inside strings, matching RFC 8259 §7.
static XrValue json_is_valid(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_bool(false);
    }

    bool strict = false;
    if (argc >= 2) {
        if (XR_IS_BOOL(args[1])) {
            strict = XR_TO_BOOL(args[1]);
        } else if (xr_value_is_json(args[1])) {
            xrs_cfg_get_bool(X, xr_value_to_json(args[1]), "strict", &strict);
        }
    }

    XrString *str = XR_TO_STRING(args[0]);
    JsonValidator v = { .ptr = str->data, .depth = 0, .ok = true, .strict = strict };

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

    // If argument is a string, parse it as JSON and return the type.
    // The previous implementation looked only at the first non-space
    // byte, so `typeOf("truefalse")` reported "boolean". We now run
    // the zero-allocation validator first and only then classify; any
    // junk after the prefix token collapses to "invalid".
    if (XR_IS_STRING(val)) {
        XrString *str = XR_TO_STRING(val);
        const char *s = str->data;

        // Skip leading whitespace for dispatch without mutating `s` for
        // the validator (which handles whitespace itself).
        const char *first = s;
        while (*first == ' ' || *first == '\t' || *first == '\n' || *first == '\r') first++;

        JsonValidator v = { .ptr = s, .depth = 0, .ok = true, .strict = false };
        validate_value(&v);
        if (v.ok) {
            validate_skip_ws(&v);
            if (*v.ptr != '\0') v.ok = false;
        }
        if (!v.ok) {
            type = "invalid";
        } else if (*first == 'n') type = "null";
        else if (*first == 't' || *first == 'f') type = "boolean";
        else if (*first == '"') type = "string";
        else if (*first == '[') type = "array";
        else if (*first == '{') type = "object";
        else type = "number";
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
    XrJsonValue *dom = xjson_parse(str->data, str->length);

    if (!dom) {
        const char *msg = "Invalid JSON";
        xr_json_set_by_key(X, result, "value", xr_null());
        xr_json_set_by_key(X, result, "error",
            xr_string_value(xr_string_intern(X, msg, strlen(msg), 0)));
    } else {
        xr_json_set_by_key(X, result, "value", dom_to_xrvalue(X, dom));
        xr_json_set_by_key(X, result, "error", xr_null());
        xjson_free(dom);
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
        if (json && xr_json_shape(X, json)) {
            XrShape *shape = xr_json_shape(X, json);
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
        if (json && xr_json_shape(X, json)) {
            XrShape *shape = xr_json_shape(X, json);
            for (uint16_t i = 0; i < shape->field_count; i++) {
                xr_array_push(values, xr_json_get_field_any(X, json, i));
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

    XRS_EXPORT(mod, isolate, "parse", json_parse);
    XRS_EXPORT(mod, isolate, "stringify", json_stringify);
    XRS_EXPORT(mod, isolate, "isValid", json_is_valid);
    XRS_EXPORT(mod, isolate, "typeof", json_type_of);
    XRS_EXPORT(mod, isolate, "tryParse", json_try_parse);
    XRS_EXPORT(mod, isolate, "keys", json_keys);
    XRS_EXPORT(mod, isolate, "values", json_values);

    // Mark as loaded
    mod->loaded = true;
    return mod;
}
