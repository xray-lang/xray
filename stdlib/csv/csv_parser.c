/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * csv_parser.c - CSV state machine parser implementation
 *
 * KEY CONCEPT:
 *   High-performance CSV parser with state machine, supporting zero-copy
 *   field extraction, SIMD acceleration, and automatic type conversion.
 */

#include "csv_parser.h"
#include "../../src/base/xmalloc.h"
#include "../common_parser.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// Unified SWAR/SIMD utility libraries
#include "xswar.h"
#include "xsimd.h"

/* ========== Initialization ========== */

void csv_config_init(CsvConfig *config) {
    memset(config, 0, sizeof(*config));
    config->delimiter = ',';
    config->quote_char = '"';
    config->escape_char = '"';
    config->header = false;
    config->columns = NULL;
    config->dynamic_typing = false;
    config->trim_fields = false;
    config->skip_empty_lines = true;
    config->has_comments = false;
    config->skip_rows = 0;
    config->max_rows = 0;
    config->relax_quotes = false;
    config->relax_columns = false;
    config->linebreak[0] = '\n';
    config->linebreak[1] = '\0';
}

void csv_config_from_json(XrayIsolate *X, CsvConfig *config, XrJson *json) {
    csv_config_init(config);
    if (!json) return;

    // Scalar fields routed through the shared config readers; they
    // silently no-op on missing / wrong-type values, preserving the
    // defaults already installed by csv_config_init().
    xrs_cfg_get_char(X, json, "delimiter",      &config->delimiter);
    xrs_cfg_get_char(X, json, "quoteChar",      &config->quote_char);
    xrs_cfg_get_char(X, json, "escapeChar",     &config->escape_char);
    xrs_cfg_get_bool(X, json, "header",         &config->header);
    xrs_cfg_get_bool(X, json, "dynamicTyping",  &config->dynamic_typing);
    xrs_cfg_get_bool(X, json, "trimFields",     &config->trim_fields);
    xrs_cfg_get_bool(X, json, "skipEmptyLines", &config->skip_empty_lines);
    xrs_cfg_get_bool(X, json, "relaxQuotes",    &config->relax_quotes);
    xrs_cfg_get_bool(X, json, "relaxColumns",   &config->relax_columns);
    xrs_cfg_get_int (X, json, "skipRows",       &config->skip_rows);
    xrs_cfg_get_int (X, json, "maxRows",        &config->max_rows);

    // columns: user-supplied header list stays as a live XrArray ref so
    // the parser can index into it without copying.
    XrValue val = xr_json_get_by_key(X, json, "columns");
    if (XR_IS_ARRAY(val)) {
        config->columns = XR_TO_ARRAY(val);
    }

    // nullStrings: override the default empty/null detection.
    // Passing `[]` explicitly disables null-detection (literal "null"
    // cells stay as strings); omitting the option keeps the built-in
    // defaults ("", "null", "NULL").
    val = xr_json_get_by_key(X, json, "nullStrings");
    if (XR_IS_ARRAY(val)) {
        config->null_strings = XR_TO_ARRAY(val);
    }

    // comments — fixed-buffer copy (parser keeps a private copy so
    // config stays valid after the caller's XrJson is released).
    size_t n = xrs_cfg_get_fixed_str(X, json, "comments",
                                     config->comments,
                                     sizeof(config->comments));
    config->has_comments = (n > 0);

    // linebreak ("\n" / "\r\n"); used by stringify only. Needs the
    // custom length guard (1..2 chars) so keep the explicit inline.
    val = xr_json_get_by_key(X, json, "linebreak");
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        if (s->length >= 1 && s->length <= 2) {
            memcpy(config->linebreak, s->data, s->length);
            config->linebreak[s->length] = '\0';
        }
    }
}

void csv_parser_init(CsvParser *parser, XrayIsolate *isolate,
                     const char *data, size_t len, CsvConfig *config) {
    memset(parser, 0, sizeof(CsvParser));

    parser->isolate = isolate;
    parser->data = data;
    parser->len = len;
    parser->pos = 0;

    parser->state = CSV_STATE_FIELD_START;
    parser->field_start = 0;
    parser->field_quoted = false;

    parser->current_row = xr_array_new(xr_current_coro(isolate));
    parser->current_row_num = 0;
    parser->current_col_num = 0;
    parser->expected_columns = -1;

    if (config) {
        parser->config = *config;
    } else {
        csv_config_init(&parser->config);
    }

    parser->result.data = xr_array_new(xr_current_coro(isolate));
    parser->result.errors = xr_array_new(xr_current_coro(isolate));
    parser->result.meta.rows = 0;
    parser->result.meta.columns = 0;
    parser->result.meta.delimiter = parser->config.delimiter;
    parser->result.meta.linebreak[0] = '\n';
    parser->result.meta.linebreak[1] = '\0';
    parser->result.meta.truncated = false;
    parser->result.meta.aborted = false;

    parser->temp_buf = NULL;
    parser->temp_len = 0;
    parser->temp_cap = 0;
}

void csv_parser_cleanup(CsvParser *parser) {
    if (parser->temp_buf) {
        xr_free(parser->temp_buf);
        parser->temp_buf = NULL;
    }
}

/* ========== Helper Functions ========== */

// Add error to result. Error Map schema matches the shared stdlib
// convention; the error-key interning is cached per isolate via
// xrs_error_push(), so repeated errors in a single parse only intern the
// type literal and the message itself.
static void add_error(CsvParser *parser, CsvErrorType type, const char *msg) {
    const char *type_str = "Unknown";
    switch (type) {
        case CSV_ERROR_UNTERMINATED_QUOTE: type_str = "UnterminatedQuote"; break;
        case CSV_ERROR_FIELD_MISMATCH:     type_str = "FieldMismatch";     break;
        case CSV_ERROR_INVALID_ESCAPE:     type_str = "InvalidEscape";     break;
        case CSV_ERROR_INVALID_ROW:        type_str = "InvalidRow";        break;
        default: break;
    }
    xrs_error_push(parser->isolate, parser->result.errors,
                   type_str,
                   /*line=*/-1,
                   /*row=*/parser->current_row_num,
                   /*column=*/parser->current_col_num,
                   msg);
}

// Ensure temp buffer capacity. Aborts on OOM rather than silently
// leaving temp_buf shorter than `needed` — previously the parser would
// carry on writing past the old tail via memcpy.
static void ensure_temp_cap(CsvParser *parser, size_t needed) {
    if (parser->temp_cap >= needed) return;

    size_t new_cap = parser->temp_cap ? parser->temp_cap * 2 : 256;
    while (new_cap < needed) new_cap *= 2;

    XR_REALLOC_OR_ABORT(parser->temp_buf, new_cap, "csv temp buffer");
    parser->temp_cap = new_cap;
}

// Trim leading and trailing whitespace
static void trim_field(const char **start, size_t *len) {
    while (*len > 0 && isspace((unsigned char)(*start)[0])) {
        (*start)++;
        (*len)--;
    }
    while (*len > 0 && isspace((unsigned char)(*start)[*len - 1])) {
        (*len)--;
    }
}

/* ========== Fast Integer Parsing (using unified SWAR library) ========== */

// Fast integer parsing with sign support, using unified xr_swar_parse_int
static bool fast_parse_int(const char *s, size_t len, int64_t *result) {
    return xr_swar_parse_int(s, len, result);
}

/* ========== Fast Float Parsing ========== */

static const double powers_of_10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
    1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static bool fast_parse_float(const char *s, size_t len, double *result) {
    if (len == 0) return false;

    bool negative = false;
    size_t i = 0;

    if (s[0] == '-') {
        negative = true;
        i = 1;
    } else if (s[0] == '+') {
        i = 1;
    }

    uint64_t int_part = 0;
    int digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        int_part = int_part * 10 + (uint64_t)(s[i] - '0');
        digits++;
        i++;
        if (digits > 18) return false;
    }

    int frac_digits = 0;
    uint64_t frac_part = 0;
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            frac_part = frac_part * 10 + (uint64_t)(s[i] - '0');
            frac_digits++;
            i++;
            if (digits + frac_digits > 18) return false;
        }
    }

    int exp = 0;
    bool exp_negative = false;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < len && s[i] == '-') {
            exp_negative = true;
            i++;
        } else if (i < len && s[i] == '+') {
            i++;
        }
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            exp = exp * 10 + (s[i] - '0');
            i++;
        }
        if (exp_negative) exp = -exp;
    }

    if (i != len) return false;

    double val = (double)int_part;
    if (frac_digits > 0 && frac_digits <= 22) {
        val += (double)frac_part / powers_of_10[frac_digits];
    }

    int total_exp = exp;
    if (total_exp != 0) {
        int abs_exp = total_exp < 0 ? -total_exp : total_exp;
        if (abs_exp > 22) return false;
        if (total_exp > 0) {
            val *= powers_of_10[abs_exp];
        } else {
            val /= powers_of_10[abs_exp];
        }
    }

    *result = negative ? -val : val;
    return true;
}

/* ========== Auto Type Conversion ========== */

XrValue csv_convert_value(XrayIsolate *isolate, const char *field, size_t len) {
    if (len == 0) {
        return xr_null();
    }

    // Boolean / null check (portable, no endian dependency)
    if (len == 4) {
        if ((field[0] | 0x20) == 't' && (field[1] | 0x20) == 'r' &&
            (field[2] | 0x20) == 'u' && (field[3] | 0x20) == 'e') {
            return xr_bool(true);
        }
        if ((field[0] | 0x20) == 'n' && (field[1] | 0x20) == 'u' &&
            (field[2] | 0x20) == 'l' && (field[3] | 0x20) == 'l') {
            return xr_null();
        }
    }
    if (len == 5) {
        if ((field[0] | 0x20) == 'f' && (field[1] | 0x20) == 'a' &&
            (field[2] | 0x20) == 'l' && (field[3] | 0x20) == 's' &&
            (field[4] | 0x20) == 'e') {
            return xr_bool(false);
        }
    }

    // Fast number parsing
    char first = field[0];
    if ((first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.') {
        int64_t int_val;
        if (fast_parse_int(field, len, &int_val)) {
            return xr_int(int_val);
        }

        double float_val;
        if (fast_parse_float(field, len, &float_val)) {
            return xr_float(float_val);
        }

        // Fallback to standard library
        if (len < 63) {
            char buf[64];
            memcpy(buf, field, len);
            buf[len] = '\0';

            char *endptr;
            double d = strtod(buf, &endptr);
            if (endptr == buf + len) {
                if (d == (double)(int64_t)d && d >= (double)INT64_MIN && d <= (double)INT64_MAX) {
                    return xr_int((int64_t)d);
                }
                return xr_float(d);
            }
        }
    }

    XrString *str = xr_string_intern(isolate, field, len, 0);
    return xr_string_value(str);
}

/* ========== Auto-detect Delimiter ========== */

char csv_detect_delimiter(const char *data, size_t len) {
    // Count occurrences of candidate delimiters
    int counts[4] = {0, 0, 0, 0};
    char candidates[4] = {',', '\t', ';', '|'};

    bool in_quote = false;
    size_t scan_len = len > 4096 ? 4096 : len;  // Only scan first 4KB

    for (size_t i = 0; i < scan_len; i++) {
        char c = data[i];

        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }

        if (in_quote) continue;

        if (c == '\n' || c == '\r') continue;

        for (int j = 0; j < 4; j++) {
            if (c == candidates[j]) {
                counts[j]++;
            }
        }
    }

    // Select the most frequent one
    int max_count = 0;
    char best = ',';
    for (int j = 0; j < 4; j++) {
        if (counts[j] > max_count) {
            max_count = counts[j];
            best = candidates[j];
        }
    }

    return best;
}

/* ========== Finish Field ========== */

static void finish_field(CsvParser *parser) {
    const char *field_data;
    size_t field_len;

    if (parser->temp_len > 0) {
        // Use temp buffer (escape processed)
        field_data = parser->temp_buf;
        field_len = parser->temp_len;
    } else {
        // Zero-copy: use original data directly
        field_data = parser->data + parser->field_start;
        field_len = parser->pos - parser->field_start;

        // If quoted field, remove surrounding quotes
        if (parser->field_quoted && field_len >= 2) {
            field_data++;
            field_len -= 2;
        }
    }

    // Trim whitespace
    if (parser->config.trim_fields) {
        trim_field(&field_data, &field_len);
    }

    // Create value.
    //
    // When dynamic_typing is active we additionally honour
    // config.null_strings: any field whose string form exactly matches
    // one of those entries is coerced to `xr_null()`. The default
    // behaviour (when the caller did not pass `nullStrings`) preserves
    // the legacy semantics — empty string maps to null via
    // csv_convert_value; every other literal stays untouched. Callers
    // that want to preserve the empty-string distinction can pass
    // `nullStrings: []` to disable null coercion entirely.
    XrValue val;
    if (parser->config.dynamic_typing) {
        XrArray *ns = parser->config.null_strings;
        bool nulled = false;
        if (ns) {
            int nscount = ns->length;
            for (int i = 0; i < nscount; i++) {
                XrValue nv = xr_array_get(ns, i);
                if (!XR_IS_STRING(nv)) continue;
                XrString *ss = XR_TO_STRING(nv);
                if (ss->length == field_len
                    && (field_len == 0
                        || memcmp(ss->data, field_data, field_len) == 0)) {
                    val = xr_null();
                    nulled = true;
                    break;
                }
            }
        }
        if (!nulled) {
            val = csv_convert_value(parser->isolate, field_data, field_len);
        }
    } else {
        XrString *str = xr_string_intern(parser->isolate, field_data, field_len, 0);
        val = xr_string_value(str);
    }

    xr_array_push(parser->current_row, val);
    parser->current_col_num++;

    // Reset field state
    parser->field_start = parser->pos + 1;
    parser->field_quoted = false;
    parser->temp_len = 0;
}

/* ========== Finish Row ========== */

static void finish_row(CsvParser *parser, XrArray *header) {
    int col_count = parser->current_row->length;

    // Empty line check
    // Fast path: a truly-empty row (no fields produced at all) must not
    // index `current_row[0]` — in debug builds `xr_array_get` asserts on
    // out-of-bounds which used to crash the parser on stray `\n\n`.
    if (col_count == 0) {
        if (parser->config.skip_empty_lines) {
            parser->current_col_num = 0;
            return;
        }
        // Fall through: treat as a zero-column row.
    } else if (col_count == 1) {
        XrValue first = xr_array_get(parser->current_row, 0);
        if (XR_IS_STRING(first)) {
            XrString *s = XR_TO_STRING(first);
            if (s->length == 0 && parser->config.skip_empty_lines) {
                parser->current_row = xr_array_new(xr_current_coro(parser->isolate));
                parser->current_col_num = 0;
                return;
            }
        }
    }

    // Comment line check
    if (parser->config.has_comments && col_count > 0) {
        XrValue first = xr_array_get(parser->current_row, 0);
        if (XR_IS_STRING(first)) {
            XrString *s = XR_TO_STRING(first);
            size_t comment_len = strlen(parser->config.comments);
            if (s->length >= comment_len &&
                strncmp(s->data, parser->config.comments, comment_len) == 0) {
                parser->current_row = xr_array_new(xr_current_coro(parser->isolate));
                parser->current_col_num = 0;
                return;
            }
        }
    }

    // Skip first N rows
    if (parser->current_row_num < parser->config.skip_rows) {
        parser->current_row = xr_array_new(xr_current_coro(parser->isolate));
        parser->current_col_num = 0;
        parser->current_row_num++;
        return;
    }

    // Column count check
    if (parser->expected_columns < 0) {
        parser->expected_columns = col_count;
        parser->result.meta.columns = col_count;
    } else if (col_count != parser->expected_columns && !parser->config.relax_columns) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Expected %d fields, got %d",
                 parser->expected_columns, col_count);
        add_error(parser, CSV_ERROR_FIELD_MISMATCH, msg);
    }

    // Add row to result
    if (header && parser->config.header) {
        // Convert to Map
        XrMap *row_map = xr_map_new(xr_current_coro(parser->isolate));
        int header_len = header->length;
        for (int i = 0; i < col_count && i < header_len; i++) {
            XrValue key = xr_array_get(header, i);
            XrValue val = xr_array_get(parser->current_row, i);
            xr_map_set(row_map, key, val);
        }
        xr_array_push(parser->result.data, xr_value_from_map(row_map));
    } else {
        xr_array_push(parser->result.data, xr_value_from_array(parser->current_row));
    }

    parser->result.meta.rows++;
    parser->current_row = xr_array_new(xr_current_coro(parser->isolate));
    parser->current_col_num = 0;
    parser->current_row_num++;

    // max_rows check
    if (parser->config.max_rows > 0 &&
        parser->result.meta.rows >= parser->config.max_rows) {
        parser->result.meta.truncated = true;
        parser->state = CSV_STATE_ERROR;
    }
}

/* ========== State Machine Parsing ========== */

// Skip UTF-8 BOM (EF BB BF) at start of data
static void skip_bom(CsvParser *parser) {
    if (parser->len >= 3 &&
        (unsigned char)parser->data[0] == 0xEF &&
        (unsigned char)parser->data[1] == 0xBB &&
        (unsigned char)parser->data[2] == 0xBF) {
        parser->pos = 3;
    }
}

// Handle row end: set header or finish row (eliminates 3x code duplication)
static void handle_row_end(CsvParser *parser, XrArray **header_ptr) {
    if (parser->config.header && !*header_ptr && parser->current_row_num == 0) {
        *header_ptr = parser->current_row;
        parser->current_row = xr_array_new(xr_current_coro(parser->isolate));
        parser->current_col_num = 0;
        parser->current_row_num++;
        parser->expected_columns = (*header_ptr)->length;
        parser->result.meta.columns = (*header_ptr)->length;
    } else {
        finish_row(parser, *header_ptr);
    }
}

// Skip \r\n and record linebreak style
static void skip_crlf(CsvParser *parser, char c) {
    if (c == '\r' && parser->pos + 1 < parser->len &&
        parser->data[parser->pos + 1] == '\n') {
        parser->pos++;
        parser->result.meta.linebreak[0] = '\r';
        parser->result.meta.linebreak[1] = '\n';
        parser->result.meta.linebreak[2] = '\0';
    }
}

void csv_parser_parse(CsvParser *parser) {
    char delim = parser->config.delimiter;
    char quote = parser->config.quote_char;
    char escape = parser->config.escape_char;

    XrArray *header = NULL;
    if (parser->config.columns) {
        header = parser->config.columns;
    }

    // Skip BOM if present
    skip_bom(parser);
    parser->field_start = parser->pos;

    while (parser->pos < parser->len) {
        char c = parser->data[parser->pos];

        switch (parser->state) {
            case CSV_STATE_ROW_END:
                break;
            case CSV_STATE_FIELD_START:
                if (c == quote) {
                    parser->field_quoted = true;
                    parser->field_start = parser->pos;
                    parser->state = CSV_STATE_QUOTED;
                } else if (c == delim) {
                    finish_field(parser);
                    parser->field_start = parser->pos + 1;
                } else if (c == '\r' || c == '\n') {
                    finish_field(parser);
                    handle_row_end(parser, &header);
                    skip_crlf(parser, c);
                    parser->field_start = parser->pos + 1;
                } else {
                    parser->state = CSV_STATE_UNQUOTED;
                }
                break;

            case CSV_STATE_UNQUOTED: {
                // SIMD fast path: batch scan to delimiter or newline
                size_t remaining = parser->len - parser->pos;
                if (remaining >= 16) {
                    const char *found = xr_simd_find_csv_delim(
                        parser->data + parser->pos, remaining, delim, quote);
                    size_t skip = found - (parser->data + parser->pos);
                    if (skip > 0) {
                        parser->pos += skip - 1;  // -1 because loop end will +1
                        break;
                    }
                }

                if (c == delim) {
                    finish_field(parser);
                    parser->state = CSV_STATE_FIELD_START;
                    parser->field_start = parser->pos + 1;
                } else if (c == '\r' || c == '\n') {
                    finish_field(parser);
                    handle_row_end(parser, &header);
                    if (parser->state == CSV_STATE_ERROR) break;
                    skip_crlf(parser, c);
                    parser->state = CSV_STATE_FIELD_START;
                    parser->field_start = parser->pos + 1;
                }
                break;
            }

            case CSV_STATE_QUOTED: {
                // SIMD fast path: scan to the next quote. When
                // escape_char == quote_char (the default RFC 4180 mode),
                // a single-character scan is safe because an escape is
                // always "quote followed by quote" and we'll re-enter the
                // slow path at the first quote anyway. When the user
                // chose a distinct escape (e.g. '\\'), the single-char
                // scan would hop straight over it and misinterpret
                // `\"` as field termination; in that case fall back to
                // a two-character find_any scan. If both chars are the
                // same (= default), we still use the cheaper single
                // scan.
                size_t remaining = parser->len - parser->pos;
                if (remaining >= 16 && c != escape && c != quote) {
                    const char *found;
                    if (escape == quote) {
                        found = xr_simd_find_char(
                            parser->data + parser->pos, remaining, quote);
                    } else {
                        char targets[2] = { quote, escape };
                        found = xr_simd_find_any(
                            parser->data + parser->pos, remaining, targets, 2);
                    }
                    size_t skip = found - (parser->data + parser->pos);
                    if (skip > 1) {
                        parser->pos += skip - 1;
                        break;
                    }
                }

                if (c == escape && parser->pos + 1 < parser->len &&
                    parser->data[parser->pos + 1] == quote) {
                    // Escaped quote: copy to temp buffer
                    size_t chunk_len = parser->pos - parser->field_start;
                    if (parser->temp_len == 0 && parser->field_quoted) {
                        // First escape, skip opening quote
                        parser->field_start++;
                        chunk_len--;
                    }
                    ensure_temp_cap(parser, parser->temp_len + chunk_len + 1);
                    memcpy(parser->temp_buf + parser->temp_len,
                           parser->data + parser->field_start, chunk_len);
                    parser->temp_len += chunk_len;
                    parser->temp_buf[parser->temp_len++] = quote;
                    parser->pos++;  // Skip escaped quote
                    parser->field_start = parser->pos + 1;
                } else if (c == quote) {
                    // Quote end
                    if (parser->temp_len > 0) {
                        // Has escaped content, append remaining
                        size_t chunk_len = parser->pos - parser->field_start;
                        ensure_temp_cap(parser, parser->temp_len + chunk_len);
                        memcpy(parser->temp_buf + parser->temp_len,
                               parser->data + parser->field_start, chunk_len);
                        parser->temp_len += chunk_len;
                    }
                    parser->state = CSV_STATE_QUOTE_END;
                }
                break;
            }

            case CSV_STATE_QUOTE_END:
                if (c == delim) {
                    finish_field(parser);
                    parser->state = CSV_STATE_FIELD_START;
                    parser->field_start = parser->pos + 1;
                } else if (c == '\r' || c == '\n') {
                    finish_field(parser);
                    handle_row_end(parser, &header);
                    if (parser->state == CSV_STATE_ERROR) break;
                    skip_crlf(parser, c);
                    parser->state = CSV_STATE_FIELD_START;
                    parser->field_start = parser->pos + 1;
                } else if (c == quote) {
                    // Double quote escape, but we already handled it in QUOTED state
                    // This may be relaxed mode case
                    if (parser->config.relax_quotes) {
                        parser->state = CSV_STATE_QUOTED;
                    } else {
                        add_error(parser, CSV_ERROR_INVALID_ESCAPE,
                                  "Unexpected quote after closing quote");
                    }
                } else {
                    // Character after closing quote
                    if (!parser->config.relax_quotes) {
                        add_error(parser, CSV_ERROR_INVALID_ESCAPE,
                                  "Unexpected character after closing quote");
                    }
                    parser->state = CSV_STATE_UNQUOTED;
                }
                break;

            case CSV_STATE_ERROR:
                // Stop parsing
                goto done;
        }

        parser->pos++;
    }

    // Handle last field
    if (parser->state == CSV_STATE_QUOTED) {
        add_error(parser, CSV_ERROR_UNTERMINATED_QUOTE, "Unterminated quote");
    }

    // If there's an incomplete field
    if (parser->pos > parser->field_start || parser->current_col_num > 0) {
        if (parser->state == CSV_STATE_UNQUOTED ||
            parser->state == CSV_STATE_QUOTE_END ||
            parser->state == CSV_STATE_FIELD_START) {
            finish_field(parser);
        }
    }

    // Handle last row
    if (parser->current_row->length > 0) {
        if (parser->config.header && !header && parser->current_row_num == 0) {
            header = parser->current_row;
        } else {
            finish_row(parser, header);
        }
    }

done:
    return;
}
