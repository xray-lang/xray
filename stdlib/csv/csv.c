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
#include "../common.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/object/xjson_serde.h"
#include "../../src/base/xmalloc.h"
#include "../common_writer.h"
#include "../common_io.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

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
    XrSerWriter sw;        // shared buffer: sw.data / sw.len / sw.cap
    XrayIsolate *isolate;  // needed for nested JSON stringify
    char delimiter;
    char quote_char;
    char escape_char;       // if != quote_char, use prefix escape
    const char *linebreak;  // "\n" or "\r\n" (owned by caller config)
    size_t linebreak_len;
} CsvWriter;

static inline void cw_init(CsvWriter *w, XrayIsolate *isolate, char delim, char quote, char escape,
                           const char *linebreak) {
    xr_serw_init(&w->sw, 256);
    w->isolate = isolate;
    w->delimiter = delim;
    w->quote_char = quote;
    w->escape_char = escape;
    w->linebreak = (linebreak && linebreak[0]) ? linebreak : "\n";
    w->linebreak_len = strlen(w->linebreak);
}

static inline void cw_free(CsvWriter *w) {
    xr_serw_free(&w->sw);
}
static inline void cw_append(CsvWriter *w, const char *s, size_t n) {
    xr_serw_append(&w->sw, s, n);
}
static inline void cw_char(CsvWriter *w, char c) {
    xr_serw_char(&w->sw, c);
}

// A field needs quoting if it contains the delimiter, quote, escape, or
// any line-terminating byte. RFC 4180 additionally recommends quoting
// leading / trailing whitespace so re-readers do not strip it; doing
// that here keeps round-trip stable across dialects that auto-trim.
static bool needs_quoting(const char *s, size_t len, char delim, char quote, char escape) {
    if (len == 0)
        return false;
    if (s[0] == ' ' || s[0] == '\t' || s[len - 1] == ' ' || s[len - 1] == '\t') {
        return true;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == delim || c == quote || c == escape || c == '\n' || c == '\r') {
            return true;
        }
    }
    return false;
}

// Write a single field with batch copy optimization.
// If escape_char != quote_char, embedded quotes are escaped with the
// configured prefix (e.g. \"). Otherwise the RFC 4180 double-quote
// form is used ("a""b"). In either case the field is wrapped in the
// quote character.
static void write_field(CsvWriter *w, const char *s, size_t len) {
    if (!needs_quoting(s, len, w->delimiter, w->quote_char, w->escape_char)) {
        cw_append(w, s, len);
        return;
    }
    cw_char(w, w->quote_char);
    bool prefix_mode = (w->escape_char != w->quote_char);
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool need_escape = (c == w->quote_char) || (prefix_mode && c == w->escape_char);
        if (need_escape) {
            if (i > start)
                cw_append(w, s + start, i - start);
            if (prefix_mode) {
                cw_char(w, w->escape_char);
                cw_char(w, c);
            } else {
                cw_char(w, w->quote_char);
                cw_char(w, w->quote_char);
            }
            start = i + 1;
        }
    }
    if (start < len)
        cw_append(w, s + start, len - start);
    cw_char(w, w->quote_char);
}

static inline void cw_newline(CsvWriter *w) {
    cw_append(w, w->linebreak, w->linebreak_len);
}

// Write a row
static void write_row(CsvWriter *w, XrArray *row) {
    int count = row->length;
    for (int i = 0; i < count; i++) {
        if (i > 0)
            cw_char(w, w->delimiter);

        XrValue val = xr_array_get(row, i);
        if (XR_IS_STRING(val)) {
            XrString *s = XR_TO_STRING(val);
            write_field(w, s->data, s->length);
        } else if (XR_IS_INT(val)) {
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%" PRId64, XR_TO_INT(val));
            cw_append(w, buf, (size_t) n);
        } else if (XR_IS_FLOAT(val)) {
            // %.17g guarantees IEEE 754 binary64 round-trip, which the
            // previous %g silently truncated (e.g. 0.1 -> "0.1" then
            // read back as 0.10000000000000001). CSV stays textual so
            // the extra digits cost at most a few bytes per field.
            double d = XR_TO_FLOAT(val);
            char buf[48];
            int n;
            if (isinf(d)) {
                n = snprintf(buf, sizeof(buf), d > 0 ? "inf" : "-inf");
            } else if (isnan(d)) {
                n = snprintf(buf, sizeof(buf), "nan");
            } else {
                n = snprintf(buf, sizeof(buf), "%.17g", d);
            }
            if (n > 0)
                cw_append(w, buf, (size_t) n);
        } else if (XR_IS_BOOL(val)) {
            if (XR_TO_BOOL(val)) {
                cw_append(w, "true", 4);
            } else {
                cw_append(w, "false", 5);
            }
        } else if (XR_IS_NULL(val)) {
            // Empty field
        } else if (XR_IS_ARRAY(val) || XR_IS_MAP(val) || xr_value_is_json(val)) {
            // Nested container: serialise inline as a JSON string and
            // emit through write_field so it gets quoted whenever the
            // payload contains delimiters / quotes / newlines. Consumer
            // parsers with dynamic_typing disabled get the raw JSON
            // string; with dynamic_typing they get a "number"/"string"
            // lookalike that most consumers still treat as string.
            //
            // Previously these rows were silently dropped (the default
            // branch wrote nothing), which is worse than losing
            // column alignment.
            size_t jlen = 0;
            char *js = xr_json_stringify_to_cstr(w->isolate, val, &jlen);
            if (js) {
                write_field(w, js, jlen);
                xr_free(js);
            }
        }
    }
    cw_newline(w);
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
    xr_map_set(meta, key_lb,
               xr_string_value(xr_string_intern(X, parser.result.meta.linebreak,
                                                strlen(parser.result.meta.linebreak), 0)));

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
    cw_init(&writer, X, config.delimiter, config.quote_char, config.escape_char, config.linebreak);

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

    XrString *result = xr_string_intern(X, writer.sw.data, writer.sw.len, 0);
    cw_free(&writer);
    return xr_string_value(result);
}

// parseFile(path) or parseFile(path, config).
// Synchronous; see stdlib/common_io.h for the P9 async plan.
static XrValue csv_parse_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }

    XrString *path = XR_TO_STRING(args[0]);

    char *content = NULL;
    size_t content_len = 0;
    if (!xrs_file_read_all_sync(path->data, &content, &content_len)) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }

    CsvConfig config;
    if (argc >= 2) {
        extract_config(X, args[1], &config);
    } else {
        csv_config_init(&config);
    }

    CsvParser parser;
    csv_parser_init(&parser, X, content, content_len, &config);
    csv_parser_parse(&parser);

    XrValue result = xr_value_from_array(parser.result.data);
    csv_parser_cleanup(&parser);
    xr_free(content);

    return result;
}

// writeFile(path, data) or writeFile(path, data, config).
// Synchronous; see stdlib/common_io.h for the P9 async plan.
static XrValue csv_write_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]) || !XR_IS_ARRAY(args[1])) {
        return xr_bool(false);
    }

    XrString *path = XR_TO_STRING(args[0]);

    // Reuse stringify
    XrValue str_args[2] = {args[1], argc >= 3 ? args[2] : xr_null()};
    XrValue csv_str = csv_stringify(X, str_args, argc >= 3 ? 2 : 1);

    if (!XR_IS_STRING(csv_str))
        return xr_bool(false);
    XrString *str = XR_TO_STRING(csv_str);

    return xr_bool(xrs_file_write_all_sync(path->data, str->data, str->length));
}

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module csv

XR_DEFINE_BUILTIN(csv_parse, "parse",
                  "(data: string, options?: Json): Array<Array<string>> | Array<Json>",
                  "Parse CSV string")
XR_DEFINE_BUILTIN(csv_parse_detailed, "parseDetailed", "(data: string, options?: Json): Json",
                  "Parse CSV with headers")
XR_DEFINE_BUILTIN(csv_parse_tsv, "parseTsv", "(data: string): Array<Array<string>>",
                  "Parse TSV string")
XR_DEFINE_BUILTIN(csv_parse_auto, "parseAuto", "(data: string): Array<Array<string>>",
                  "Auto-detect delimiter and parse")
XR_DEFINE_BUILTIN(csv_stringify, "stringify",
                  "(data: Array<Array<string>>, options?: Json): string", "Convert to CSV string")
XR_DEFINE_BUILTIN(csv_parse_file, "parseFile",
                  "(path: string, options?: Json): Array<Array<string>>", "Parse CSV file")
XR_DEFINE_BUILTIN(csv_write_file, "writeFile",
                  "(path: string, data: Array<Array<string>>, options?: Json): bool",
                  "Write CSV file")

XR_FUNC XrModule *xr_load_module_csv(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "csv");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "parse", csv_parse);
    XRS_EXPORT(mod, isolate, "parseDetailed", csv_parse_detailed);
    XRS_EXPORT(mod, isolate, "parseTsv", csv_parse_tsv);
    XRS_EXPORT(mod, isolate, "parseAuto", csv_parse_auto);
    XRS_EXPORT(mod, isolate, "stringify", csv_stringify);
    XRS_EXPORT(mod, isolate, "parseFile", csv_parse_file);
    XRS_EXPORT(mod, isolate, "writeFile", csv_write_file);

    mod->loaded = true;
    return mod;
}
