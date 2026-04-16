/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * csv.c - CSV standard library implementation
 *
 * KEY CONCEPT:
 *   High-performance CSV parser with state machine. Configuration via Json.
 */

#include "csv.h"
#include "csv_parser.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

/* ========== Helper Functions ========== */

// Extract config from Json object
static void extract_config(XrayIsolate *X, XrValue config_val, CsvConfig *config) {
    csv_config_init(config);
    
    if (xr_value_is_json(config_val)) {
        XrJson *json = xr_value_to_json(config_val);
        csv_config_from_json(X, config, json);
    }
}

/* ========== CSV Serialization ========== */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    char delimiter;
    char quote_char;
} CsvWriter;

static void cw_init(CsvWriter *w, char delim, char quote) {
    w->cap = 256;
    w->buf = (char*)xr_malloc(w->cap);
    if (!w->buf) { w->cap = 0; w->len = 0; return; }
    w->len = 0;
    w->delimiter = delim;
    w->quote_char = quote;
}

static void cw_append(CsvWriter *w, const char *s, size_t n) {
    if (w->len + n >= w->cap) {
        while (w->len + n >= w->cap) w->cap *= 2;
        char *nb = (char*)xr_realloc(w->buf, w->cap);
        if (!nb) return;
        w->buf = nb;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void cw_char(CsvWriter *w, char c) {
    cw_append(w, &c, 1);
}

// Check if field needs quoting
static bool needs_quoting(const char *s, size_t len, char delim, char quote) {
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == delim || c == quote || c == '\n' || c == '\r') {
            return true;
        }
    }
    return false;
}

// Write a single field with batch copy optimization
static void write_field(CsvWriter *w, const char *s, size_t len) {
    if (needs_quoting(s, len, w->delimiter, w->quote_char)) {
        cw_char(w, w->quote_char);
        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == w->quote_char) {
                // Flush batch before quote
                if (i > start) cw_append(w, s + start, i - start);
                cw_char(w, w->quote_char);
                cw_char(w, w->quote_char);
                start = i + 1;
            }
        }
        // Flush remaining batch
        if (start < len) cw_append(w, s + start, len - start);
        cw_char(w, w->quote_char);
    } else {
        cw_append(w, s, len);
    }
}

// Write a row
static void write_row(CsvWriter *w, XrArray *row) {
    int count = row->length;
    for (int i = 0; i < count; i++) {
        if (i > 0) cw_char(w, w->delimiter);
        
        XrValue val = xr_array_get(row, i);
        if (XR_IS_STRING(val)) {
            XrString *s = XR_TO_STRING(val);
            write_field(w, s->data, s->length);
        } else if (XR_IS_INT(val)) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%" PRId64, XR_TO_INT(val));
            cw_append(w, buf, (size_t)n);
        } else if (XR_IS_FLOAT(val)) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%g", XR_TO_FLOAT(val));
            cw_append(w, buf, (size_t)n);
        } else if (XR_IS_BOOL(val)) {
            if (XR_TO_BOOL(val)) {
                cw_append(w, "true", 4);
            } else {
                cw_append(w, "false", 5);
            }
        } else if (XR_IS_NULL(val)) {
            // Empty field
        }
    }
    cw_char(w, '\n');
}

/* ========== Module Functions ========== */

// parse(str) or parse(str, config)
static XrValue csv_parse(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    
    XrString *str = XR_TO_STRING(args[0]);
    
    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }
    
    CsvParser parser;
    csv_parser_init(&parser, X, str->data, str->length, &config);
    csv_parser_parse(&parser);
    
    XrValue result = xr_value_from_array(parser.result.data);
    csv_parser_cleanup(&parser);
    
    return result;
}

// parseDetailed(str, config) - returns detailed result
static XrValue csv_parse_detailed(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        XrMap *result = xr_map_new(xr_current_coro(X));
        XrValue key_data = xr_string_value(xr_string_intern(X, "data", 4, 0));
        xr_map_set(result, key_data, xr_value_from_array(xr_array_new(xr_current_coro(X))));
        return xr_value_from_map(result);
    }
    
    XrString *str = XR_TO_STRING(args[0]);
    
    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }
    
    CsvParser parser;
    csv_parser_init(&parser, X, str->data, str->length, &config);
    csv_parser_parse(&parser);
    
    // Build result object
    XrMap *result = xr_map_new(xr_current_coro(X));
    
    // data
    XrValue key_data = xr_string_value(xr_string_intern(X, "data", 4, 0));
    xr_map_set(result, key_data, xr_value_from_array(parser.result.data));
    
    // errors
    XrValue key_errors = xr_string_value(xr_string_intern(X, "errors", 6, 0));
    xr_map_set(result, key_errors, xr_value_from_array(parser.result.errors));
    
    // meta
    XrMap *meta = xr_map_new(xr_current_coro(X));
    
    XrValue key_rows = xr_string_value(xr_string_intern(X, "rows", 4, 0));
    xr_map_set(meta, key_rows, xr_int(parser.result.meta.rows));
    
    XrValue key_cols = xr_string_value(xr_string_intern(X, "columns", 7, 0));
    xr_map_set(meta, key_cols, xr_int(parser.result.meta.columns));
    
    XrValue key_delim = xr_string_value(xr_string_intern(X, "delimiter", 9, 0));
    char delim_str[2] = {parser.result.meta.delimiter, '\0'};
    xr_map_set(meta, key_delim, xr_string_value(xr_string_intern(X, delim_str, 1, 0)));
    
    XrValue key_lb = xr_string_value(xr_string_intern(X, "linebreak", 9, 0));
    xr_map_set(meta, key_lb, xr_string_value(xr_string_intern(X, 
        parser.result.meta.linebreak, strlen(parser.result.meta.linebreak), 0)));
    
    XrValue key_trunc = xr_string_value(xr_string_intern(X, "truncated", 9, 0));
    xr_map_set(meta, key_trunc, xr_bool(parser.result.meta.truncated));
    
    XrValue key_meta = xr_string_value(xr_string_intern(X, "meta", 4, 0));
    xr_map_set(result, key_meta, xr_value_from_map(meta));
    
    csv_parser_cleanup(&parser);
    
    return xr_value_from_map(result);
}

// parseTsv(str) - TSV shortcut
static XrValue csv_parse_tsv(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    
    XrString *str = XR_TO_STRING(args[0]);
    
    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }
    config.delimiter = '\t';  // Always force tab delimiter for TSV
    
    CsvParser parser;
    csv_parser_init(&parser, X, str->data, str->length, &config);
    csv_parser_parse(&parser);
    
    XrValue result = xr_value_from_array(parser.result.data);
    csv_parser_cleanup(&parser);
    
    return result;
}

// parseAuto(str) - auto-detect delimiter
static XrValue csv_parse_auto(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    
    XrString *str = XR_TO_STRING(args[0]);
    
    // Auto-detect delimiter
    char detected = csv_detect_delimiter(str->data, str->length);
    
    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }
    config.delimiter = detected;
    
    CsvParser parser;
    csv_parser_init(&parser, X, str->data, str->length, &config);
    csv_parser_parse(&parser);
    
    XrValue result = xr_value_from_array(parser.result.data);
    csv_parser_cleanup(&parser);
    
    return result;
}

// stringify(data) or stringify(data, config)
static XrValue csv_stringify(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_ARRAY(args[0])) {
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    }
    
    XrArray *data = XR_TO_ARRAY(args[0]);
    
    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }
    
    CsvWriter writer;
    cw_init(&writer, config.delimiter, config.quote_char);
    
    // Check if array of Maps (need to extract headers)
    XrArray *headers = NULL;
    bool is_map_array = false;
    
    if (data->length > 0) {
        XrValue first = xr_array_get(data, 0);
        if (XR_IS_MAP(first)) {
            is_map_array = true;
            // Use public API to extract keys
            headers = xr_map_keys(xr_current_coro(X), XR_TO_MAP(first));
            
            // Write header row
            if (config.header) {
                write_row(&writer, headers);
            }
        }
    }
    
    // Write data rows
    for (int i = 0; i < data->length; i++) {
        XrValue row_val = xr_array_get(data, i);
        
        if (is_map_array && XR_IS_MAP(row_val)) {
            XrMap *map = XR_TO_MAP(row_val);
            XrArray *row = xr_array_new(xr_current_coro(X));
            
            int header_count = headers->length;
            for (int j = 0; j < header_count; j++) {
                XrValue key = xr_array_get(headers, j);
                XrValue val = xr_map_get(map, key, NULL);
                xr_array_push(row, val);
            }
            write_row(&writer, row);
        } else if (XR_IS_ARRAY(row_val)) {
            write_row(&writer, XR_TO_ARRAY(row_val));
        }
    }
    
    XrString *result = xr_string_intern(X, writer.buf, writer.len, 0);
    xr_free(writer.buf);
    return xr_string_value(result);
}

// parseFile(path) or parseFile(path, config)
static XrValue csv_parse_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    
    XrString *path = XR_TO_STRING(args[0]);
    
    FILE *f = fopen(path->data, "rb");
    if (!f) return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    fseek(f, 0, SEEK_SET);
    
    char *content = (char*)xr_malloc((size_t)size + 1);
    if (!content) {
        fclose(f);
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    size_t read_size = fread(content, 1, (size_t)size, f);
    content[read_size] = '\0';
    fclose(f);
    
    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }
    
    CsvParser parser;
    csv_parser_init(&parser, X, content, read_size, &config);
    csv_parser_parse(&parser);
    
    XrValue result = xr_value_from_array(parser.result.data);
    csv_parser_cleanup(&parser);
    xr_free(content);
    
    return result;
}

// writeFile(path, data) or writeFile(path, data, config)
static XrValue csv_write_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_ARRAY(args[1])) {
        return xr_bool(false);
    }
    
    XrString *path = XR_TO_STRING(args[0]);
    
    // Reuse stringify
    XrValue str_args[2] = {args[1], argc >= 3 ? args[2] : xr_null()};
    XrValue csv_str = csv_stringify(X, str_args, argc >= 3 ? 2 : 1);
    
    if (!XR_IS_STRING(csv_str)) return xr_bool(false);
    XrString *str = XR_TO_STRING(csv_str);
    
    FILE *f = fopen(path->data, "wb");
    if (!f) return xr_bool(false);
    fwrite(str->data, 1, str->length, f);
    fclose(f);
    
    return xr_bool(true);
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module csv

XR_DEFINE_BUILTIN(csv_parse, "parse", "(data: string, options?: Json): Array<Array<string>>", "Parse CSV string")
XR_DEFINE_BUILTIN(csv_parse_detailed, "parseDetailed", "(data: string, options?: Json): Json", "Parse CSV with headers")
XR_DEFINE_BUILTIN(csv_parse_tsv, "parseTsv", "(data: string): Array<Array<string>>", "Parse TSV string")
XR_DEFINE_BUILTIN(csv_parse_auto, "parseAuto", "(data: string): Array<Array<string>>", "Auto-detect delimiter and parse")
XR_DEFINE_BUILTIN(csv_stringify, "stringify", "(data: Array<Array<string>>, options?: Json): string", "Convert to CSV string")
XR_DEFINE_BUILTIN(csv_parse_file, "parseFile", "(path: string, options?: Json): Array<Array<string>>", "Parse CSV file")
XR_DEFINE_BUILTIN(csv_write_file, "writeFile", "(path: string, data: Array<Array<string>>, options?: Json): bool", "Write CSV file")

XrModule* xr_load_module_csv(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "csv");
    if (!mod) return NULL;
    
    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);
    
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)
    
    EXPORT_CFUNC("parse", csv_parse);
    EXPORT_CFUNC("parseDetailed", csv_parse_detailed);
    EXPORT_CFUNC("parseTsv", csv_parse_tsv);
    EXPORT_CFUNC("parseAuto", csv_parse_auto);
    EXPORT_CFUNC("stringify", csv_stringify);
    EXPORT_CFUNC("parseFile", csv_parse_file);
    EXPORT_CFUNC("writeFile", csv_write_file);
    
    #undef EXPORT_CFUNC
    
    mod->loaded = true;
    return mod;
}
