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
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// Unified SWAR/SIMD utility libraries
#include "xswar.h"
#include "xsimd.h"

/* ========== Initialization ========== */

void csv_config_init(CsvConfig *config) {
    config->delimiter = ',';
    config->quote_char = '"';
    config->escape_char = '"';
    config->header = false;
    config->columns = NULL;
    config->dynamic_typing = false;
    config->trim_fields = false;
    config->skip_empty_lines = true;
    config->comments = NULL;
    config->skip_rows = 0;
    config->max_rows = 0;
    config->relax_quotes = false;
    config->relax_columns = false;
}

void csv_config_from_json(XrayIsolate *X, CsvConfig *config, XrJson *json) {
    csv_config_init(config);
    if (!json) return;
    
    // delimiter
    XrValue val = xr_json_get_by_key(X, json, "delimiter");
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        if (s->length > 0) config->delimiter = s->data[0];
    }
    
    // quoteChar
    val = xr_json_get_by_key(X, json, "quoteChar");
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        if (s->length > 0) config->quote_char = s->data[0];
    }
    
    // escapeChar
    val = xr_json_get_by_key(X, json, "escapeChar");
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        if (s->length > 0) config->escape_char = s->data[0];
    }
    
    // header
    val = xr_json_get_by_key(X, json, "header");
    if (XR_IS_BOOL(val)) {
        config->header = XR_TO_BOOL(val);
    }
    
    // columns
    val = xr_json_get_by_key(X, json, "columns");
    if (XR_IS_ARRAY(val)) {
        config->columns = XR_TO_ARRAY(val);
    }
    
    // dynamicTyping
    val = xr_json_get_by_key(X, json, "dynamicTyping");
    if (XR_IS_BOOL(val)) {
        config->dynamic_typing = XR_TO_BOOL(val);
    }
    
    // trimFields
    val = xr_json_get_by_key(X, json, "trimFields");
    if (XR_IS_BOOL(val)) {
        config->trim_fields = XR_TO_BOOL(val);
    }
    
    // skipEmptyLines
    val = xr_json_get_by_key(X, json, "skipEmptyLines");
    if (XR_IS_BOOL(val)) {
        config->skip_empty_lines = XR_TO_BOOL(val);
    }
    
    // comments
    val = xr_json_get_by_key(X, json, "comments");
    if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        config->comments = s->data;
    }
    
    // skipRows
    val = xr_json_get_by_key(X, json, "skipRows");
    if (XR_IS_INT(val)) {
        config->skip_rows = (int)XR_TO_INT(val);
    }
    
    // maxRows
    val = xr_json_get_by_key(X, json, "maxRows");
    if (XR_IS_INT(val)) {
        config->max_rows = (int)XR_TO_INT(val);
    }
    
    // relaxQuotes
    val = xr_json_get_by_key(X, json, "relaxQuotes");
    if (XR_IS_BOOL(val)) {
        config->relax_quotes = XR_TO_BOOL(val);
    }
    
    // relaxColumns
    val = xr_json_get_by_key(X, json, "relaxColumns");
    if (XR_IS_BOOL(val)) {
        config->relax_columns = XR_TO_BOOL(val);
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

// Add error to result
static void add_error(CsvParser *parser, CsvErrorType type, const char *msg) {
    XrMap *err = xr_map_new(xr_current_coro(parser->isolate));
    
    const char *type_str = "Unknown";
    switch (type) {
        case CSV_ERROR_UNTERMINATED_QUOTE: type_str = "UnterminatedQuote"; break;
        case CSV_ERROR_FIELD_MISMATCH: type_str = "FieldMismatch"; break;
        case CSV_ERROR_INVALID_ESCAPE: type_str = "InvalidEscape"; break;
        case CSV_ERROR_INVALID_ROW: type_str = "InvalidRow"; break;
        default: break;
    }
    
    XrValue key_type = xr_string_value(xr_string_intern(parser->isolate, "type", 4, 0));
    XrValue val_type = xr_string_value(xr_string_intern(parser->isolate, type_str, strlen(type_str), 0));
    xr_map_set(err, key_type, val_type);
    
    XrValue key_row = xr_string_value(xr_string_intern(parser->isolate, "row", 3, 0));
    xr_map_set(err, key_row, xr_int(parser->current_row_num));
    
    XrValue key_col = xr_string_value(xr_string_intern(parser->isolate, "column", 6, 0));
    xr_map_set(err, key_col, xr_int(parser->current_col_num));
    
    XrValue key_msg = xr_string_value(xr_string_intern(parser->isolate, "message", 7, 0));
    XrValue val_msg = xr_string_value(xr_string_intern(parser->isolate, msg, strlen(msg), 0));
    xr_map_set(err, key_msg, val_msg);
    
    xr_array_push(parser->result.errors, xr_value_from_map(err));
}

// Ensure temp buffer capacity
static void ensure_temp_cap(CsvParser *parser, size_t needed) {
    if (parser->temp_cap >= needed) return;
    
    size_t new_cap = parser->temp_cap ? parser->temp_cap * 2 : 256;
    while (new_cap < needed) new_cap *= 2;
    
    char *nb = (char*)xr_realloc(parser->temp_buf, new_cap);
    if (!nb) return;
    parser->temp_buf = nb;
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
    
    // Create value
    XrValue val;
    if (parser->config.dynamic_typing) {
        val = csv_convert_value(parser->isolate, field_data, field_len);
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
    if (col_count == 0 || (col_count == 1 && parser->current_row->length == 1)) {
        XrValue first = xr_array_get(parser->current_row, 0);
        if (XR_IS_STRING(first)) {
            XrString *s = XR_TO_STRING(first);
            if (s->length == 0) {
                if (parser->config.skip_empty_lines) {
                    parser->current_row = xr_array_new(xr_current_coro(parser->isolate));
                    parser->current_col_num = 0;
                    return;
                }
            }
        }
    }
    
    // Comment line check
    if (parser->config.comments && col_count > 0) {
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
                // SIMD fast path: scan to quote or escape char
                size_t remaining = parser->len - parser->pos;
                if (remaining >= 16 && c != escape && c != quote) {
                    const char *found = xr_simd_find_char(
                        parser->data + parser->pos, remaining, quote);
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
