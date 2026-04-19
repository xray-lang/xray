/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * toml.c - TOML standard library implementation
 *
 * KEY CONCEPT:
 *   Implements TOML v1.0.0 specification parsing and serialization.
 */

#include "toml.h"
#include "toml_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../common_writer.h"
#include "../datetime/datetime.h"
#include "../../src/base/xmalloc.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/runtime/xisolate_internal.h"

// ========== Parser Wrapper ==========

XrValue xr_toml_parse(XrayIsolate *X, const char *data, size_t len) {
    TomlConfig config;
    toml_config_init(&config);

    TomlParser parser;
    toml_parser_init(&parser, X, data, len, &config);
    TomlResult result = toml_parser_parse(&parser);
    toml_parser_cleanup(&parser);

    return xr_value_from_map(result.data);
}

// ========== TOML Serialization ==========

typedef struct {
    XrSerWriter sw;       // shared buffer: sw.data / sw.len / sw.cap
    XrayIsolate *isolate;
    int indent;
    int depth;            // nesting depth for stringify cycle guard
} TomlWriter;

#define TOML_STRINGIFY_MAX_DEPTH 256

static inline void tw_init(TomlWriter *w, XrayIsolate *isolate, int indent) {
    xr_serw_init(&w->sw, 256);
    w->isolate = isolate;
    w->indent = indent;
    w->depth = 0;
}

static inline void tw_free(TomlWriter *w) {
    xr_serw_free(&w->sw);
}

static inline void tw_append(TomlWriter *w, const char *s, size_t n) {
    xr_serw_append(&w->sw, s, n);
}

static inline void tw_str(TomlWriter *w, const char *s) {
    xr_serw_str(&w->sw, s);
}

static inline void tw_char(TomlWriter *w, char c) {
    xr_serw_char(&w->sw, c);
}

// Check if key is a valid TOML bare key (only [A-Za-z0-9_-])
static bool is_bare_key(const char *key, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; i++) {
        char c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

// Write TOML basic-string escape for a single byte. TOML spec forbids
// raw control characters (U+0000..U+0008, U+000B..U+001F) in basic
// strings; emit \uXXXX for them. In multiline mode we still escape the
// same range because line-endings are handled separately by the caller.
static void tw_escape_byte(TomlWriter *w, unsigned char c) {
    switch (c) {
        case '"':  tw_str(w, "\\\""); return;
        case '\\': tw_str(w, "\\\\"); return;
        case '\b': tw_str(w, "\\b"); return;
        case '\f': tw_str(w, "\\f"); return;
        case '\n': tw_str(w, "\\n"); return;
        case '\r': tw_str(w, "\\r"); return;
        case '\t': tw_str(w, "\\t"); return;
    }
    if (c < 0x20 || c == 0x7F) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04X", c);
        tw_str(w, buf);
        return;
    }
    tw_char(w, (char)c);
}

static void write_string_value(TomlWriter *w, XrString *str) {
    // Use multiline form only when the string actually contains '\n';
    // always escape other control chars so the output stays
    // spec-compliant on round-trip.
    bool has_newline = false;
    for (size_t i = 0; i < str->length; i++) {
        if (str->data[i] == '\n') { has_newline = true; break; }
    }

    if (has_newline) {
        tw_str(w, "\"\"\"\n");
        for (size_t i = 0; i < str->length; i++) {
            unsigned char c = (unsigned char)str->data[i];
            // Real newlines stay as-is inside a literal triple-quoted
            // block (they are part of the payload).
            if (c == '\n') { tw_char(w, '\n'); continue; }
            // Escape sequences of three+ quotes inside multiline
            if (c == '"' && i + 2 < str->length
                && str->data[i + 1] == '"' && str->data[i + 2] == '"') {
                tw_str(w, "\\\"\\\"\\\"");
                i += 2;
                continue;
            }
            tw_escape_byte(w, c);
        }
        tw_str(w, "\"\"\"");
    } else {
        tw_char(w, '"');
        for (size_t i = 0; i < str->length; i++) {
            tw_escape_byte(w, (unsigned char)str->data[i]);
        }
        tw_char(w, '"');
    }
}

// Write a TOML key, quoting if necessary
static void write_key(TomlWriter *w, XrString *key) {
    if (is_bare_key(key->data, key->length)) {
        tw_append(w, key->data, key->length);
    } else {
        write_string_value(w, key);
    }
}

static void write_value(TomlWriter *w, XrValue val);
static void write_table(TomlWriter *w, XrMap *map, const char *prefix);

static void write_array(TomlWriter *w, XrArray *arr) {
    tw_char(w, '[');
    for (int i = 0; i < arr->length; i++) {
        if (i > 0) tw_str(w, ", ");
        write_value(w, xr_array_get(arr, i));
    }
    tw_char(w, ']');
}

static void write_value(TomlWriter *w, XrValue val) {
    if (w->depth >= TOML_STRINGIFY_MAX_DEPTH) {
        // Depth guard prevents infinite recursion on cyclic maps.
        tw_str(w, "\"\"");
        return;
    }
    if (XR_IS_NULL(val)) {
        // TOML has no null type; skip null values in table context,
        // output empty string as fallback in array/inline context
        tw_str(w, "\"\"");
        return;
    }
    if (XR_IS_BOOL(val)) {
        tw_str(w, XR_TO_BOOL(val) ? "true" : "false");
    } else if (XR_IS_INT(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)XR_TO_INT(val));
        tw_str(w, buf);
    } else if (XR_IS_FLOAT(val)) {
        double d = XR_TO_FLOAT(val);
        char buf[64];
        if (isinf(d)) {
            tw_str(w, d > 0 ? "inf" : "-inf");
        } else if (isnan(d)) {
            tw_str(w, "nan");
        } else {
            // %.17g guarantees IEEE 754 binary64 round-trip.
            snprintf(buf, sizeof(buf), "%.17g", d);
            // TOML requires float to have a decimal point or exponent
            if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
                size_t l = strlen(buf);
                if (l + 2 < sizeof(buf)) {
                    buf[l] = '.';
                    buf[l + 1] = '0';
                    buf[l + 2] = '\0';
                }
            }
            tw_str(w, buf);
        }
    } else if (XR_IS_DATETIME(val)) {
        // Emit as TOML offset / local date-time in RFC 3339 format.
        char buf[64];
        int n = xr_datetime_to_iso_string(XR_TO_DATETIME(val), buf, sizeof(buf));
        if (n > 0) tw_append(w, buf, (size_t)n);
        else tw_str(w, "\"\"");
    } else if (XR_IS_STRING(val)) {
        write_string_value(w, XR_TO_STRING(val));
    } else if (XR_IS_ARRAY(val)) {
        write_array(w, XR_TO_ARRAY(val));
    } else if (xr_value_is_json(val)) {
        // XrJson -> inline table form {k1 = v1, k2 = v2, ...}
        XrJson *json = xr_value_to_json(val);
        XrShape *shape = xr_json_shape(json);
        XrSymbolTable *symtab = (XrSymbolTable*)w->isolate->symbol_table;
        tw_char(w, '{');
        w->depth++;
        if (shape) {
            bool first = true;
            for (uint16_t i = 0; i < shape->field_count; i++) {
                SymbolId sym = shape->field_symbols[i];
                const char *name = xr_symbol_get_name_in_table(symtab, sym);
                if (!name) continue;
                if (!first) tw_str(w, ", ");
                first = false;
                size_t nlen = strlen(name);
                if (is_bare_key(name, nlen)) {
                    tw_append(w, name, nlen);
                } else {
                    // Quote using basic-string escape.
                    tw_char(w, '"');
                    for (size_t k = 0; k < nlen; k++) {
                        tw_escape_byte(w, (unsigned char)name[k]);
                    }
                    tw_char(w, '"');
                }
                tw_str(w, " = ");
                write_value(w, xr_json_get_field_any(json, i));
            }
        }
        w->depth--;
        tw_char(w, '}');
    } else if (XR_IS_MAP(val)) {
        XrMap *map = XR_TO_MAP(val);
        tw_char(w, '{');
        if (!xr_map_isdummy(map)) {
            uint32_t size = xr_map_sizenode(map);
            bool first = true;
            w->depth++;
            for (uint32_t i = 0; i < size; i++) {
                XrMapNode *node = xr_map_node(map, i);
                if (XR_MAP_NODE_EMPTY(node)) continue;
                if (!first) tw_str(w, ", ");
                first = false;
                if (XR_IS_STRING(node->key)) {
                    write_key(w, XR_TO_STRING(node->key));
                }
                tw_str(w, " = ");
                write_value(w, node->value);
            }
            w->depth--;
        }
        tw_char(w, '}');
    }
}

// Build dotted prefix: "parent.child" or just "child"
// Caller releases via xr_free().
static char* make_prefix(const char *prefix, XrString *key) {
    char *result;
    if (prefix && prefix[0]) {
        size_t plen = strlen(prefix);
        size_t total = plen + key->length + 2; // '.' + NUL
        result = (char*)xr_malloc(total);
        if (!result) return NULL;
        snprintf(result, total, "%s.%.*s", prefix, (int)key->length, key->data);
    } else {
        result = (char*)xr_malloc(key->length + 1);
        if (!result) return NULL;
        memcpy(result, key->data, key->length);
        result[key->length] = '\0';
    }
    return result;
}

// Check if value is an array-of-tables (array whose first element is a Map)
static bool is_array_of_tables(XrValue val) {
    if (!XR_IS_ARRAY(val)) return false;
    XrArray *arr = XR_TO_ARRAY(val);
    return arr->length > 0 && XR_IS_MAP(xr_array_get(arr, 0));
}

static void write_table(TomlWriter *w, XrMap *map, const char *prefix) {
    if (xr_map_isdummy(map)) return;
    uint32_t size = xr_map_sizenode(map);

    // Pass 1: simple key-value pairs
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (XR_MAP_NODE_EMPTY(node)) continue;
        if (XR_IS_MAP(node->value) || is_array_of_tables(node->value)) continue;
        if (XR_IS_STRING(node->key)) {
            write_key(w, XR_TO_STRING(node->key));
        }
        tw_str(w, " = ");
        write_value(w, node->value);
        tw_char(w, '\n');
    }

    // Pass 2: sub-tables [key]
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (XR_MAP_NODE_EMPTY(node) || !XR_IS_MAP(node->value)) continue;
        XrString *key = XR_IS_STRING(node->key) ? XR_TO_STRING(node->key) : NULL;
        if (!key) continue;

        char *new_prefix = make_prefix(prefix, key);
        tw_char(w, '\n');
        tw_char(w, '[');
        tw_str(w, new_prefix);
        tw_str(w, "]\n");
        write_table(w, XR_TO_MAP(node->value), new_prefix);
        xr_free(new_prefix);
    }

    // Pass 3: array-of-tables [[key]]
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (XR_MAP_NODE_EMPTY(node) || !is_array_of_tables(node->value)) continue;
        XrString *key = XR_IS_STRING(node->key) ? XR_TO_STRING(node->key) : NULL;
        if (!key) continue;

        char *new_prefix = make_prefix(prefix, key);
        XrArray *arr = XR_TO_ARRAY(node->value);
        for (int j = 0; j < arr->length; j++) {
            XrValue elem = xr_array_get(arr, j);
            if (XR_IS_MAP(elem)) {
                tw_char(w, '\n');
                tw_str(w, "[[");
                tw_str(w, new_prefix);
                tw_str(w, "]]\n");
                write_table(w, XR_TO_MAP(elem), new_prefix);
            }
        }
        xr_free(new_prefix);
    }
}

XrValue xr_toml_stringify(XrayIsolate *isolate, XrValue value, int indent) {
    if (!XR_IS_MAP(value)) {
        return xr_string_value(xr_string_intern(isolate, "", 0, 0));
    }

    TomlWriter writer;
    tw_init(&writer, isolate, indent);

    write_table(&writer, XR_TO_MAP(value), "");

    XrString *result = xr_string_intern(isolate, writer.sw.data, writer.sw.len, 0);
    tw_free(&writer);
    return xr_string_value(result);
}

// ========== Module Functions ==========

static XrValue toml_parse(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_map(xr_map_new(xr_current_coro(X)));
    }
    XrString *str = XR_TO_STRING(args[0]);

    TomlConfig config;
    toml_config_init(&config);

    if (argc >= 2 && xr_value_is_json(args[1])) {
        XrJson *json = xr_value_to_json(args[1]);
        toml_config_from_json(X, &config, json);
    }

    TomlParser parser;
    toml_parser_init(&parser, X, str->data, str->length, &config);
    TomlResult result = toml_parser_parse(&parser);
    toml_parser_cleanup(&parser);

    return xr_value_from_map(result.data);
}

static XrValue toml_parse_strict(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        XrMap *result = xr_map_new(xr_current_coro(X));
        xr_map_set(result,
            xr_string_value(xr_string_intern(X, "data", 4, 0)),
            xr_value_from_map(xr_map_new(xr_current_coro(X))));
        xr_map_set(result,
            xr_string_value(xr_string_intern(X, "errors", 6, 0)),
            xr_value_from_array(xr_array_new(xr_current_coro(X))));
        return xr_value_from_map(result);
    }
    XrString *str = XR_TO_STRING(args[0]);

    TomlConfig config;
    toml_config_init(&config);
    config.strict = true;

    TomlParser parser;
    toml_parser_init(&parser, X, str->data, str->length, &config);
    TomlResult parse_result = toml_parser_parse(&parser);
    toml_parser_cleanup(&parser);

    XrMap *result = xr_map_new(xr_current_coro(X));
    xr_map_set(result,
        xr_string_value(xr_string_intern(X, "data", 4, 0)),
        xr_value_from_map(parse_result.data));
    xr_map_set(result,
        xr_string_value(xr_string_intern(X, "errors", 6, 0)),
        xr_value_from_array(parse_result.errors));

    XrMap *meta = xr_map_new(xr_current_coro(X));
    xr_map_set(meta,
        xr_string_value(xr_string_intern(X, "lines", 5, 0)),
        xr_int(parse_result.meta.lines));
    xr_map_set(meta,
        xr_string_value(xr_string_intern(X, "keys", 4, 0)),
        xr_int(parse_result.meta.keys));
    xr_map_set(result,
        xr_string_value(xr_string_intern(X, "meta", 4, 0)),
        xr_value_from_map(meta));

    return xr_value_from_map(result);
}

static XrValue toml_stringify(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    }
    int indent = 0;

    if (argc >= 2 && xr_value_is_json(args[1])) {
        XrJson *json = xr_value_to_json(args[1]);
        XrValue val = xr_json_get_by_key(X, json, "indent");
        if (XR_IS_INT(val)) {
            indent = (int)XR_TO_INT(val);
        }
    } else if (argc >= 2 && XR_IS_INT(args[1])) {
        indent = (int)XR_TO_INT(args[1]);
    }

    return xr_toml_stringify(X, args[0], indent);
}

static XrValue toml_parse_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_map(xr_map_new(xr_current_coro(X)));
    }
    XrString *path = XR_TO_STRING(args[0]);

    FILE *f = fopen(path->data, "rb");
    if (!f) {
        return xr_value_from_map(xr_map_new(xr_current_coro(X)));
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return xr_value_from_map(xr_map_new(xr_current_coro(X)));
    }
    fseek(f, 0, SEEK_SET);

    char *content = (char*)xr_malloc(size + 1);
    if (!content) {
        fclose(f);
        return xr_value_from_map(xr_map_new(xr_current_coro(X)));
    }
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    XrValue result = xr_toml_parse(X, content, read_size);
    xr_free(content);
    return result;
}

static XrValue toml_write_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0])) {
        return xr_bool(false);
    }
    XrString *path = XR_TO_STRING(args[0]);
    XrValue toml_str = xr_toml_stringify(X, args[1], 0);

    if (!XR_IS_STRING(toml_str)) {
        return xr_bool(false);
    }
    XrString *str = XR_TO_STRING(toml_str);

    FILE *f = fopen(path->data, "wb");
    if (!f) {
        return xr_bool(false);
    }
    fwrite(str->data, 1, str->length, f);
    fclose(f);
    return xr_bool(true);
}

// ========== Module Loading ==========

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module toml

XR_DEFINE_BUILTIN(toml_parse, "parse", "(data: string): Json?", "Parse TOML string")
XR_DEFINE_BUILTIN(toml_parse_strict, "parseStrict", "(data: string): Json?", "Parse TOML strictly")
XR_DEFINE_BUILTIN(toml_stringify, "stringify", "(value: Json): string", "Convert to TOML string")
XR_DEFINE_BUILTIN(toml_parse_file, "parseFile", "(path: string): Json?", "Parse TOML file")
XR_DEFINE_BUILTIN(toml_write_file, "writeFile", "(path: string, value: Json): bool", "Write TOML file")

XrModule* xr_load_module_toml(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "toml");
    if (!mod) return NULL;

    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)

    EXPORT_CFUNC("parse", toml_parse);
    EXPORT_CFUNC("parseStrict", toml_parse_strict);
    EXPORT_CFUNC("stringify", toml_stringify);
    EXPORT_CFUNC("parseFile", toml_parse_file);
    EXPORT_CFUNC("writeFile", toml_write_file);

    #undef EXPORT_CFUNC

    mod->loaded = true;
    return mod;
}
