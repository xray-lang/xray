/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * toml_parser.c - TOML state machine parser implementation
 *
 * KEY CONCEPT:
 *   High-performance TOML parser.
 */

#include "toml_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../datetime/datetime.h"
#include "../../src/base/xmalloc.h"
#include "../../src/runtime/value/xvalue.h"
#include "../common_parser.h"

// Unified SWAR utility library
#include "xswar.h"

// ========== Fast Number Parsing (using unified SWAR library) ==========

static bool fast_parse_int(const char *s, size_t len, int64_t *result) {
    return xr_swar_parse_int(s, len, result);
}

// ========== Helper Functions ==========

// Forward declaration so add_expect_error can call add_error, which is
// defined a few lines below.
static void add_error(TomlParser *parser, TomlErrorType type, const char *msg);

// Format a "Expected X but got Y" syntax diagnostic. Keeps the human
// context visible in long files where "Expected ']'" alone is not
// enough to locate the offending token. If the input is at EOF we say
// so explicitly rather than printing whatever `PEEK()` happens to
// return. The message is still routed through add_error so every
// syntax site ends up in the errors array with a consistent schema.
static void add_expect_error(TomlParser *parser, const char *expected) {
    char buf[128];
    if (parser->pos >= parser->len) {
        snprintf(buf, sizeof(buf),
                 "Expected %s but reached end of input", expected);
    } else {
        unsigned char c = (unsigned char)parser->data[parser->pos];
        if (c >= 0x20 && c < 0x7F) {
            snprintf(buf, sizeof(buf),
                     "Expected %s but got '%c'", expected, c);
        } else {
            snprintf(buf, sizeof(buf),
                     "Expected %s but got byte 0x%02X", expected, c);
        }
    }
    add_error(parser, TOML_ERROR_SYNTAX, buf);
}

static void add_error(TomlParser *parser, TomlErrorType type, const char *msg) {
    const char *type_str = "Unknown";
    switch (type) {
        case TOML_ERROR_SYNTAX:              type_str = "SyntaxError";        break;
        case TOML_ERROR_DUPLICATE_KEY:       type_str = "DuplicateKey";       break;
        case TOML_ERROR_INVALID_KEY:         type_str = "InvalidKey";         break;
        case TOML_ERROR_INVALID_VALUE:       type_str = "InvalidValue";       break;
        case TOML_ERROR_UNTERMINATED_STRING: type_str = "UnterminatedString"; break;
        case TOML_ERROR_INVALID_ESCAPE:      type_str = "InvalidEscape";      break;
        case TOML_ERROR_INVALID_NUMBER:      type_str = "InvalidNumber";      break;
        case TOML_ERROR_INVALID_DATETIME:    type_str = "InvalidDatetime";    break;
        case TOML_ERROR_TABLE_CONFLICT:      type_str = "TableConflict";      break;
        default: break;
    }
    xrs_error_push(parser->isolate, parser->result.errors,
                   type_str,
                   /*line=*/parser->line,
                   /*row=*/-1,
                   /*column=*/parser->column,
                   msg);
}

static void ensure_buf_cap(TomlParser *parser, size_t needed) {
    if (parser->temp_cap >= needed) return;
    size_t new_cap = parser->temp_cap ? parser->temp_cap * 2 : 64;
    while (new_cap < needed) new_cap *= 2;
    // Abort instead of silently returning — downstream buf_append_char
    // and memcpy would otherwise write past the old tail on OOM.
    XR_REALLOC_OR_ABORT(parser->temp_buf, new_cap, "toml parser temp buffer");
    parser->temp_cap = new_cap;
}

static void buf_append_char(TomlParser *parser, char c) {
    ensure_buf_cap(parser, parser->temp_len + 2);
    parser->temp_buf[parser->temp_len++] = c;
}

static void buf_reset(TomlParser *parser) {
    parser->temp_len = 0;
}

// ========== Initialization ==========

void toml_config_init(TomlConfig *config) {
    config->strict = false;
    config->allow_duplicate_keys = false;
}

void toml_config_from_json(XrayIsolate *X, TomlConfig *config, XrJson *json) {
    toml_config_init(config);
    if (!json) return;

    XrValue val = xr_json_get_by_key(X, json, "strict");
    if (XR_IS_BOOL(val)) {
        config->strict = XR_TO_BOOL(val);
    }

    val = xr_json_get_by_key(X, json, "allowDuplicateKeys");
    if (XR_IS_BOOL(val)) {
        config->allow_duplicate_keys = XR_TO_BOOL(val);
    }
}

void toml_parser_init(TomlParser *parser, XrayIsolate *isolate,
                      const char *data, size_t len, TomlConfig *config) {
    memset(parser, 0, sizeof(TomlParser));

    parser->isolate = isolate;
    parser->data = data;
    parser->len = len;
    parser->pos = 0;
    parser->line = 1;
    parser->column = 1;
    parser->state = TOML_STATE_LINE_START;

    parser->root = xr_map_new(xr_current_coro(isolate));
    parser->current_table = parser->root;
    parser->current_key_path = xr_array_new(xr_current_coro(isolate));
    parser->defined_tables = xr_map_new(xr_current_coro(isolate));

    if (config) {
        parser->config = *config;
    } else {
        toml_config_init(&parser->config);
    }

    parser->result.data = parser->root;
    parser->result.errors = xr_array_new(xr_current_coro(isolate));
    parser->result.meta.lines = 0;
    parser->result.meta.keys = 0;
}

void toml_parser_cleanup(TomlParser *parser) {
    if (parser->temp_buf) {
        xr_free(parser->temp_buf);
        parser->temp_buf = NULL;
    }
}

// ========== Parsing Helpers ==========

#define PEEK() (parser->pos < parser->len ? parser->data[parser->pos] : '\0')
#define ADVANCE() do { parser->pos++; parser->column++; } while(0)
#define AT_END() (parser->pos >= parser->len)

static void skip_ws(TomlParser *parser) {
    while (!AT_END() && (PEEK() == ' ' || PEEK() == '\t')) {
        ADVANCE();
    }
}

static void skip_ws_and_newlines(TomlParser *parser) {
    while (!AT_END()) {
        char c = PEEK();
        if (c == ' ' || c == '\t') {
            ADVANCE();
        } else if (c == '\n') {
            ADVANCE();
            parser->line++;
            parser->column = 1;
        } else if (c == '\r') {
            ADVANCE();
            if (!AT_END() && PEEK() == '\n') ADVANCE();
            parser->line++;
            parser->column = 1;
        } else if (c == '#') {
            while (!AT_END() && PEEK() != '\n') ADVANCE();
        } else {
            break;
        }
    }
}

static void skip_to_eol(TomlParser *parser) {
    while (!AT_END() && PEEK() != '\n' && PEEK() != '\r') {
        ADVANCE();
    }
}

static bool is_bare_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

// ========== Forward Declarations ==========

static XrValue parse_value(TomlParser *parser);
static XrArray* parse_key_path(TomlParser *parser);
static void set_nested_value(TomlParser *parser, XrMap *root,
                             XrArray *keys, XrValue val);

// ========== Value Parsing ==========

static XrValue parse_bare_key(TomlParser *parser) {
    const char *start = parser->data + parser->pos;
    size_t len = 0;

    while (!AT_END() && is_bare_key_char(PEEK())) {
        ADVANCE();
        len++;
    }

    if (len == 0) {
        add_error(parser, TOML_ERROR_INVALID_KEY, "Expected key name");
        return xr_null();
    }

    XrString *str = xr_string_intern(parser->isolate, start, len, 0);
    return xr_string_value(str);
}

static XrValue parse_basic_string(TomlParser *parser) {
    if (PEEK() != '"') return xr_null();
    ADVANCE();

    bool multiline = false;
    if (!AT_END() && PEEK() == '"') {
        ADVANCE();
        if (!AT_END() && PEEK() == '"') {
            multiline = true;
            ADVANCE();
            if (!AT_END() && PEEK() == '\n') {
                ADVANCE();
                parser->line++;
                parser->column = 1;
            }
        } else {
            XrString *str = xr_string_intern(parser->isolate, "", 0, 0);
            return xr_string_value(str);
        }
    }

    buf_reset(parser);

    while (!AT_END()) {
        if (multiline) {
            if (PEEK() == '"' && parser->pos + 2 < parser->len &&
                parser->data[parser->pos + 1] == '"' &&
                parser->data[parser->pos + 2] == '"') {
                parser->pos += 3;
                parser->column += 3;
                break;
            }
        } else {
            if (PEEK() == '"') {
                ADVANCE();
                break;
            }
            if (PEEK() == '\n' || PEEK() == '\r') {
                add_error(parser, TOML_ERROR_UNTERMINATED_STRING, "Unterminated string");
                return xr_null();
            }
        }

        if (PEEK() == '\\') {
            ADVANCE();
            if (AT_END()) break;

            char c = PEEK();
            ADVANCE();

            switch (c) {
                case 'n': buf_append_char(parser, '\n'); break;
                case 't': buf_append_char(parser, '\t'); break;
                case 'r': buf_append_char(parser, '\r'); break;
                case '\\': buf_append_char(parser, '\\'); break;
                case '"': buf_append_char(parser, '"'); break;
                case 'b': buf_append_char(parser, '\b'); break;
                case 'f': buf_append_char(parser, '\f'); break;
                case 'u': case 'U': {
                    int digits = (c == 'u') ? 4 : 8;
                    unsigned int cp = 0;
                    int actual = 0;
                    for (int i = 0; i < digits && !AT_END(); i++) {
                        char h = PEEK();
                        if ((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')) {
                            ADVANCE();
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else cp |= h - 'A' + 10;
                            actual++;
                        } else {
                            break;
                        }
                    }
                    if (actual != digits) {
                        add_error(parser, TOML_ERROR_INVALID_ESCAPE,
                                  c == 'u' ? "\\u requires exactly 4 hex digits"
                                           : "\\U requires exactly 8 hex digits");
                        break;
                    }
                    // Reject surrogate pairs and out-of-range codepoints
                    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                        add_error(parser, TOML_ERROR_INVALID_ESCAPE, "Invalid Unicode codepoint");
                        break;
                    }
                    if (cp < 0x80) {
                        buf_append_char(parser, (char)cp);
                    } else if (cp < 0x800) {
                        buf_append_char(parser, (char)(0xC0 | (cp >> 6)));
                        buf_append_char(parser, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        buf_append_char(parser, (char)(0xE0 | (cp >> 12)));
                        buf_append_char(parser, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        buf_append_char(parser, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        buf_append_char(parser, (char)(0xF0 | (cp >> 18)));
                        buf_append_char(parser, (char)(0x80 | ((cp >> 12) & 0x3F)));
                        buf_append_char(parser, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        buf_append_char(parser, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                case '\n': case '\r':
                    if (multiline) {
                        if (c == '\r' && !AT_END() && PEEK() == '\n') ADVANCE();
                        parser->line++;
                        parser->column = 1;
                        while (!AT_END() && (PEEK() == ' ' || PEEK() == '\t' ||
                               PEEK() == '\n' || PEEK() == '\r')) {
                            if (PEEK() == '\n') {
                                parser->line++;
                                parser->column = 1;
                            }
                            ADVANCE();
                        }
                    }
                    break;
                default:
                    buf_append_char(parser, c);
                    break;
            }
        } else {
            if (PEEK() == '\n') {
                parser->line++;
                parser->column = 1;
            }
            buf_append_char(parser, PEEK());
            ADVANCE();
        }
    }

    XrString *str = xr_string_intern(parser->isolate,
                                      parser->temp_buf ? parser->temp_buf : "",
                                      parser->temp_len, 0);
    return xr_string_value(str);
}

static XrValue parse_literal_string(TomlParser *parser) {
    if (PEEK() != '\'') return xr_null();
    ADVANCE();

    bool multiline = false;
    if (!AT_END() && PEEK() == '\'') {
        ADVANCE();
        if (!AT_END() && PEEK() == '\'') {
            multiline = true;
            ADVANCE();
            if (!AT_END() && PEEK() == '\n') {
                ADVANCE();
                parser->line++;
                parser->column = 1;
            }
        } else {
            XrString *str = xr_string_intern(parser->isolate, "", 0, 0);
            return xr_string_value(str);
        }
    }

    const char *start = parser->data + parser->pos;
    size_t len = 0;

    while (!AT_END()) {
        if (multiline) {
            if (PEEK() == '\'' && parser->pos + 2 < parser->len &&
                parser->data[parser->pos + 1] == '\'' &&
                parser->data[parser->pos + 2] == '\'') {
                parser->pos += 3;
                parser->column += 3;
                break;
            }
        } else {
            if (PEEK() == '\'') {
                ADVANCE();
                break;
            }
            if (PEEK() == '\n' || PEEK() == '\r') {
                add_error(parser, TOML_ERROR_UNTERMINATED_STRING, "Unterminated string");
                return xr_null();
            }
        }
        if (PEEK() == '\n') {
            parser->line++;
            parser->column = 1;
        }
        ADVANCE();
        len++;
    }

    XrString *str = xr_string_intern(parser->isolate, start, len, 0);
    return xr_string_value(str);
}

static XrValue parse_number(TomlParser *parser) {
    const char *start = parser->data + parser->pos;
    bool is_float = false;
    bool is_hex = false;
    bool is_oct = false;
    bool is_bin = false;
    bool negative = false;

    if (PEEK() == '+' || PEEK() == '-') {
        negative = (PEEK() == '-');
        ADVANCE();
    }

    if (parser->pos + 3 <= parser->len) {
        if (strncmp(parser->data + parser->pos, "inf", 3) == 0) {
            parser->pos += 3;
            parser->column += 3;
            return xr_float(negative ? -INFINITY : INFINITY);
        }
        if (strncmp(parser->data + parser->pos, "nan", 3) == 0) {
            parser->pos += 3;
            parser->column += 3;
            return xr_float(NAN);
        }
    }

    if (parser->pos + 1 < parser->len && parser->data[parser->pos] == '0') {
        char next = parser->data[parser->pos + 1];
        if (next == 'x' || next == 'X') {
            is_hex = true;
            parser->pos += 2;
            parser->column += 2;
        } else if (next == 'o' || next == 'O') {
            is_oct = true;
            parser->pos += 2;
            parser->column += 2;
        } else if (next == 'b' || next == 'B') {
            is_bin = true;
            parser->pos += 2;
            parser->column += 2;
        }
    }

    // TOML spec forbids a '+'/'-' sign on hex / oct / bin literals.
    // Example: '-0xFF' must be rejected.
    if (negative && (is_hex || is_oct || is_bin)) {
        add_error(parser, TOML_ERROR_INVALID_NUMBER,
                  "sign prefix only allowed on decimal numbers");
    }

    // Track the previous non-underscore character so we can validate
    // the TOML underscore rule (must be surrounded by digits on both
    // sides, i.e. '_1', '1_', '1__2' are all rejected).
    char prev_digit = 0;
    bool bad_underscore = false;
    while (!AT_END()) {
        char c = PEEK();
        if (c == '_') {
            // Leading underscore (no preceding digit) or double underscore
            if (prev_digit == 0 || prev_digit == '_') {
                bad_underscore = true;
            }
            // Peek the next char: must be a valid digit for the current
            // base, otherwise the underscore is trailing / before non-digit.
            char next = (parser->pos + 1 < parser->len) ? parser->data[parser->pos + 1] : 0;
            bool next_ok = false;
            if (is_hex) next_ok = isxdigit((unsigned char)next);
            else if (is_oct) next_ok = (next >= '0' && next <= '7');
            else if (is_bin) next_ok = (next == '0' || next == '1');
            else next_ok = isdigit((unsigned char)next);
            if (!next_ok) bad_underscore = true;
            prev_digit = '_';
            ADVANCE();
            continue;
        }
        if (is_hex) {
            if (!isxdigit((unsigned char)c)) break;
        } else if (is_oct) {
            if (c < '0' || c > '7') break;
        } else if (is_bin) {
            if (c != '0' && c != '1') break;
        } else {
            if (!isdigit((unsigned char)c) && c != '.' &&
                c != 'e' && c != 'E' && c != '+' && c != '-') break;
            if (c == '.' || c == 'e' || c == 'E') is_float = true;
        }
        prev_digit = c;
        ADVANCE();
    }
    if (bad_underscore) {
        add_error(parser, TOML_ERROR_INVALID_NUMBER,
                  "underscore must be surrounded by digits");
    }

    // Strip underscores into temp_buf (reuse parser buffer, no malloc per number)
    size_t raw_len = (parser->data + parser->pos) - start;
    buf_reset(parser);
    ensure_buf_cap(parser, raw_len + 1);
    for (size_t i = 0; i < raw_len; i++) {
        if (start[i] != '_') {
            parser->temp_buf[parser->temp_len++] = start[i];
        }
    }
    parser->temp_buf[parser->temp_len] = '\0';
    char *num_buf = parser->temp_buf;
    size_t num_len = parser->temp_len;

    XrValue result;
    if (is_hex) {
        int64_t val = strtoll(num_buf + (negative ? 3 : 2), NULL, 16);
        result = xr_int(negative ? -val : val);
    } else if (is_oct) {
        int64_t val = strtoll(num_buf + (negative ? 3 : 2), NULL, 8);
        result = xr_int(negative ? -val : val);
    } else if (is_bin) {
        int64_t val = strtoll(num_buf + (negative ? 3 : 2), NULL, 2);
        result = xr_int(negative ? -val : val);
    } else if (is_float) {
        double val = strtod(num_buf, NULL);
        result = xr_float(val);
    } else {
        int64_t val;
        if (fast_parse_int(num_buf, num_len, &val)) {
            result = xr_int(val);
        } else {
            result = xr_int(strtoll(num_buf, NULL, 10));
        }
    }

    return result;
}

static XrValue parse_datetime(TomlParser *parser) {
    const char *start = parser->data + parser->pos;
    bool found_date_part = false;

    while (!AT_END()) {
        char c = PEEK();
        if (isdigit((unsigned char)c) || c == '-' || c == ':' ||
            c == 'T' || c == 't' || c == 'Z' || c == 'z' ||
            c == '+' || c == '.') {
            if (c == 'T' || c == 't') found_date_part = true;
            ADVANCE();
        } else if (c == ' ' && !found_date_part) {
            // TOML v1.0: space can replace T between date and time
            // e.g. "1979-05-27 07:32:00"
            size_t ahead = parser->pos + 1;
            if (ahead < parser->len && isdigit((unsigned char)parser->data[ahead])) {
                found_date_part = true;
                ADVANCE();
            } else {
                break;
            }
        } else {
            break;
        }
    }

    size_t len = (parser->data + parser->pos) - start;

    // TOML datetimes are a first-class type. Parse into XrDateTime via the
    // shared datetime helper; if the payload is not a recognisable
    // ISO-8601 shape (e.g. a bare local time or date), fall back to a
    // string so callers can still inspect the literal.
    // xr_datetime_parse expects NUL-terminated input, so copy out.
    char small[64];
    char *buf = small;
    if (len + 1 > sizeof(small)) {
        buf = (char*)xr_malloc(len + 1);
        if (!buf) {
            XrString *str = xr_string_intern(parser->isolate, start, len, 0);
            return xr_string_value(str);
        }
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    XrDateTime *dt = xr_datetime_parse(parser->isolate, buf, NULL);
    if (buf != small) xr_free(buf);
    if (dt) {
        return xr_datetime_value(dt);
    }

    // Fallback: keep original literal as a string.
    XrString *str = xr_string_intern(parser->isolate, start, len, 0);
    return xr_string_value(str);
}

static XrValue parse_array(TomlParser *parser) {
    if (PEEK() != '[') return xr_null();
    ADVANCE();

    XrArray *arr = xr_array_new(xr_current_coro(parser->isolate));

    skip_ws_and_newlines(parser);

    if (!AT_END() && PEEK() == ']') {
        ADVANCE();
        return xr_value_from_array(arr);
    }

    while (!AT_END()) {
        skip_ws_and_newlines(parser);

        XrValue val = parse_value(parser);
        xr_array_push(arr, val);

        skip_ws_and_newlines(parser);

        if (AT_END()) break;

        if (PEEK() == ']') {
            ADVANCE();
            break;
        }

        if (PEEK() == ',') {
            ADVANCE();
            continue;
        }

        add_expect_error(parser, "',' or ']'");
        break;
    }

    return xr_value_from_array(arr);
}

static XrValue parse_inline_table(TomlParser *parser) {
    if (PEEK() != '{') return xr_null();
    ADVANCE();

    XrMap *map = xr_map_new(xr_current_coro(parser->isolate));

    skip_ws(parser);

    if (!AT_END() && PEEK() == '}') {
        ADVANCE();
        return xr_value_from_map(map);
    }

    while (!AT_END()) {
        skip_ws(parser);

        // Support dotted keys in inline tables: {a.b = 1}
        XrArray *keys = parse_key_path(parser);

        skip_ws(parser);

        if (AT_END() || PEEK() != '=') {
            add_expect_error(parser, "'='");
            break;
        }
        ADVANCE();

        skip_ws(parser);

        XrValue val = parse_value(parser);
        set_nested_value(parser, map, keys, val);

        skip_ws(parser);

        if (AT_END()) break;

        if (PEEK() == '}') {
            ADVANCE();
            break;
        }

        if (PEEK() == ',') {
            ADVANCE();
            continue;
        }

        add_expect_error(parser, "',' or '}'");
        break;
    }

    return xr_value_from_map(map);
}

static XrValue parse_value(TomlParser *parser) {
    skip_ws(parser);

    if (AT_END()) {
        add_error(parser, TOML_ERROR_SYNTAX, "Expected value");
        return xr_null();
    }

    char c = PEEK();

    if (c == '"') return parse_basic_string(parser);
    if (c == '\'') return parse_literal_string(parser);

    if (parser->pos + 4 <= parser->len &&
        strncmp(parser->data + parser->pos, "true", 4) == 0 &&
        (parser->pos + 4 >= parser->len || !isalnum((unsigned char)parser->data[parser->pos + 4]))) {
        parser->pos += 4;
        parser->column += 4;
        return xr_bool(true);
    }
    if (parser->pos + 5 <= parser->len &&
        strncmp(parser->data + parser->pos, "false", 5) == 0 &&
        (parser->pos + 5 >= parser->len || !isalnum((unsigned char)parser->data[parser->pos + 5]))) {
        parser->pos += 5;
        parser->column += 5;
        return xr_bool(false);
    }

    if (c == '[') return parse_array(parser);
    if (c == '{') return parse_inline_table(parser);

    // Bare inf/nan (without +/- prefix)
    if (parser->pos + 3 <= parser->len) {
        if (strncmp(parser->data + parser->pos, "inf", 3) == 0 &&
            (parser->pos + 3 >= parser->len || !isalnum((unsigned char)parser->data[parser->pos + 3]))) {
            parser->pos += 3;
            parser->column += 3;
            return xr_float(INFINITY);
        }
        if (strncmp(parser->data + parser->pos, "nan", 3) == 0 &&
            (parser->pos + 3 >= parser->len || !isalnum((unsigned char)parser->data[parser->pos + 3]))) {
            parser->pos += 3;
            parser->column += 3;
            return xr_float(NAN);
        }
    }

    if (isdigit((unsigned char)c) || c == '+' || c == '-') {
        if (parser->pos + 10 <= parser->len && isdigit((unsigned char)c)) {
            const char *p = parser->data + parser->pos;
            if (isdigit(p[0]) && isdigit(p[1]) && isdigit(p[2]) &&
                isdigit(p[3]) && p[4] == '-') {
                return parse_datetime(parser);
            }
        }
        return parse_number(parser);
    }

    add_error(parser, TOML_ERROR_INVALID_VALUE, "Invalid value");
    return xr_null();
}

// ========== Key Path Parsing ==========

static XrArray* parse_key_path(TomlParser *parser) {
    XrArray *keys = xr_array_new(xr_current_coro(parser->isolate));

    while (!AT_END()) {
        skip_ws(parser);

        XrValue key;
        if (PEEK() == '"') {
            key = parse_basic_string(parser);
        } else if (PEEK() == '\'') {
            key = parse_literal_string(parser);
        } else {
            key = parse_bare_key(parser);
        }

        xr_array_push(keys, key);

        skip_ws(parser);

        if (AT_END() || PEEK() != '.') break;
        ADVANCE();
    }

    return keys;
}

// ========== Table Operations ==========

static void set_nested_value(TomlParser *parser, XrMap *root,
                             XrArray *keys, XrValue val) {
    XrMap *current = root;
    int count = keys->length;

    for (int i = 0; i < count - 1; i++) {
        XrValue key = xr_array_get(keys, i);
        XrValue existing = xr_map_get(current, key, NULL);

        if (XR_IS_NULL(existing)) {
            XrMap *new_map = xr_map_new(xr_current_coro(parser->isolate));
            xr_map_set(current, key, xr_value_from_map(new_map));
            current = new_map;
        } else if (XR_IS_MAP(existing)) {
            current = XR_TO_MAP(existing);
        } else {
            XrMap *new_map = xr_map_new(xr_current_coro(parser->isolate));
            xr_map_set(current, key, xr_value_from_map(new_map));
            current = new_map;
        }
    }

    if (count > 0) {
        XrValue last_key = xr_array_get(keys, count - 1);

        // TOML spec: "You cannot define any key or table more than once".
        // Previously the parser logged an error but still called
        // xr_map_set which silently overrode the first value; keep the
        // first binding so the error matches user expectations.
        if (!parser->config.allow_duplicate_keys) {
            XrValue existing = xr_map_get(current, last_key, NULL);
            if (!XR_IS_NULL(existing)) {
                add_error(parser, TOML_ERROR_DUPLICATE_KEY, "Duplicate key");
                return;
            }
        }

        xr_map_set(current, last_key, val);
        parser->result.meta.keys++;
    }
}

// Join the array-of-XrString keys into a dotted path string using the
// parser's reusable temp buffer. Returns the NUL-terminated path; the
// buffer lives until the next buf_reset / ensure_buf_cap call, so the
// caller must consume it before any other buffer operation.
static const char* build_table_path(TomlParser *parser, XrArray *keys) {
    buf_reset(parser);
    int n = keys->length;
    for (int i = 0; i < n; i++) {
        XrValue v = xr_array_get(keys, i);
        if (!XR_IS_STRING(v)) continue;
        XrString *s = XR_TO_STRING(v);
        if (i > 0) buf_append_char(parser, '.');
        ensure_buf_cap(parser, parser->temp_len + s->length + 1);
        memcpy(parser->temp_buf + parser->temp_len, s->data, s->length);
        parser->temp_len += s->length;
    }
    ensure_buf_cap(parser, parser->temp_len + 1);
    parser->temp_buf[parser->temp_len] = '\0';
    return parser->temp_buf;
}

// Record a standard-header path into the defined_tables set.
// Returns true if this was the first time we saw it, false if the
// header is a duplicate of a previous `[path]` declaration.
static bool mark_table_defined(TomlParser *parser, XrArray *keys) {
    const char *path = build_table_path(parser, keys);
    XrString *pkey = xr_string_intern(parser->isolate, path,
                                      parser->temp_len, 0);
    XrValue key = xr_string_value(pkey);
    if (!XR_IS_NULL(xr_map_get(parser->defined_tables, key, NULL))) {
        return false;  // already seen
    }
    xr_map_set(parser->defined_tables, key, xr_bool(true));
    return true;
}

static XrMap* get_or_create_table(TomlParser *parser, XrArray *keys) {
    // Duplicate-header detection (TOML spec §Table): two `[same.path]`
    // headers in the same document are an error. Dotted-key implicit
    // tables are intentionally NOT recorded in defined_tables, so
    // `a.b = 1` followed by `[a]` remains legal.
    if (!mark_table_defined(parser, keys)) {
        add_error(parser, TOML_ERROR_DUPLICATE_KEY,
                  "Table already defined");
        // Return the existing leaf so subsequent key/value lines still
        // land in a sensible place and the rest of the document keeps
        // parsing; the error has already been recorded.
    }

    XrMap *current = parser->root;
    int count = keys->length;

    for (int i = 0; i < count; i++) {
        XrValue key = xr_array_get(keys, i);
        XrValue existing = xr_map_get(current, key, NULL);

        if (XR_IS_NULL(existing)) {
            XrMap *new_map = xr_map_new(xr_current_coro(parser->isolate));
            xr_map_set(current, key, xr_value_from_map(new_map));
            current = new_map;
        } else if (XR_IS_MAP(existing)) {
            current = XR_TO_MAP(existing);
        } else {
            add_error(parser, TOML_ERROR_TABLE_CONFLICT, "Table definition conflict");
            return parser->root;
        }
    }

    return current;
}

static XrMap* get_or_create_array_table(TomlParser *parser, XrArray *keys) {
    XrMap *current = parser->root;
    int count = keys->length;

    for (int i = 0; i < count - 1; i++) {
        XrValue key = xr_array_get(keys, i);
        XrValue existing = xr_map_get(current, key, NULL);

        if (XR_IS_NULL(existing)) {
            XrMap *new_map = xr_map_new(xr_current_coro(parser->isolate));
            xr_map_set(current, key, xr_value_from_map(new_map));
            current = new_map;
        } else if (XR_IS_MAP(existing)) {
            current = XR_TO_MAP(existing);
        } else if (XR_IS_ARRAY(existing)) {
            XrArray *arr = XR_TO_ARRAY(existing);
            if (arr->length > 0) {
                XrValue last = xr_array_get(arr, arr->length - 1);
                if (XR_IS_MAP(last)) {
                    current = XR_TO_MAP(last);
                } else {
                    return parser->root;
                }
            } else {
                return parser->root;
            }
        } else {
            return parser->root;
        }
    }

    if (count > 0) {
        XrValue last_key = xr_array_get(keys, count - 1);
        XrValue existing = xr_map_get(current, last_key, NULL);

        XrArray *arr;
        if (XR_IS_NULL(existing)) {
            arr = xr_array_new(xr_current_coro(parser->isolate));
            xr_map_set(current, last_key, xr_value_from_array(arr));
        } else if (XR_IS_ARRAY(existing)) {
            arr = XR_TO_ARRAY(existing);
        } else {
            return parser->root;
        }

        XrMap *new_table = xr_map_new(xr_current_coro(parser->isolate));
        xr_array_push(arr, xr_value_from_map(new_table));
        return new_table;
    }

    return current;
}

// ========== Main Parsing Function ==========

TomlResult toml_parser_parse(TomlParser *parser) {
    while (!AT_END()) {
        skip_ws_and_newlines(parser);
        if (AT_END()) break;

        if (PEEK() == '[') {
            ADVANCE();

            bool is_array_table = false;
            if (!AT_END() && PEEK() == '[') {
                is_array_table = true;
                ADVANCE();
            }

            skip_ws(parser);

            XrArray *keys = parse_key_path(parser);

            skip_ws(parser);

            if (AT_END() || PEEK() != ']') {
                add_expect_error(parser, "']'");
                skip_to_eol(parser);
                continue;
            }
            ADVANCE();

            if (is_array_table) {
                if (AT_END() || PEEK() != ']') {
                    add_expect_error(parser, "']]'");
                    skip_to_eol(parser);
                    continue;
                }
                ADVANCE();
                parser->current_table = get_or_create_array_table(parser, keys);
            } else {
                parser->current_table = get_or_create_table(parser, keys);
            }

            skip_to_eol(parser);
            continue;
        }

        XrArray *keys = parse_key_path(parser);

        skip_ws(parser);

        if (AT_END() || PEEK() != '=') {
            add_expect_error(parser, "'='");
            skip_to_eol(parser);
            continue;
        }
        ADVANCE();

        skip_ws(parser);

        XrValue val = parse_value(parser);
        set_nested_value(parser, parser->current_table, keys, val);

        skip_to_eol(parser);
    }

    parser->result.meta.lines = parser->line;
    return parser->result;
}

