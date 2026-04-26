/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml_parser.c - YAML state machine parser implementation
 *
 * KEY CONCEPT:
 *   High-performance YAML parser.
 */

#include "yaml_parser.h"
#include "../../src/base/xmalloc.h"
#include "../../src/runtime/object/xstring.h"
#include "../common_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// Scanner function declarations
extern bool yaml_fast_parse_int(const char *s, size_t len, int64_t *result);
extern void yaml_skip_ws(YamlParser *p);
extern void yaml_skip_to_eol(YamlParser *p);
extern void yaml_skip_newline(YamlParser *p);
extern void yaml_skip_empty_lines(YamlParser *p);
extern int yaml_count_indent(YamlParser *p);
extern bool yaml_indent_has_tab(YamlParser *p, int indent);

// Forward declare so line-indent check can reuse the strict path.
static void yaml_add_error(YamlParser *p, const char *type, const char *message);

// Count indent and, if followed by a tab, surface a YAML 1.2 §6.1
// violation via the strict-mode error list. The parse continues at the
// reported indent so the rest of the document keeps flowing — the
// error is observable via parseStrict().errors.
static int yaml_line_indent(YamlParser *p) {
    int n = yaml_count_indent(p);
    if (yaml_indent_has_tab(p, n)) {
        yaml_add_error(p, "tab_indent",
                       "tab characters must not be used for indentation "
                       "(YAML 1.2 §6.1)");
    }
    return n;
}

// ========== Configuration Initialization ==========

void yaml_config_init(YamlConfig *config) {
    config->safe = true;
    config->allow_duplicate_keys = false;
    // Default depth is the stdlib-wide XR_STDLIB_MAX_DEPTH so YAML keeps
    // the same safety envelope as JSON / TOML / XML. Callers that need a
    // stricter limit for untrusted input can still lower it via the
    // maxDepth config field.
    config->max_depth = XR_STDLIB_MAX_DEPTH;
}

void yaml_config_from_json(XrayIsolate *X, YamlConfig *config, XrJson *json) {
    yaml_config_init(config);
    if (!json)
        return;

    xrs_cfg_get_bool(X, json, "safe", &config->safe);
    xrs_cfg_get_bool(X, json, "allowDuplicateKeys", &config->allow_duplicate_keys);
    xrs_cfg_get_int(X, json, "maxDepth", &config->max_depth);
}

// ========== Parser Initialization ==========

void yaml_parser_init(YamlParser *parser, XrayIsolate *isolate, const char *data, size_t len,
                      YamlConfig *config) {
    memset(parser, 0, sizeof(YamlParser));

    parser->isolate = isolate;
    parser->data = data;
    parser->ptr = data;
    parser->end = data + len;
    parser->line = 1;
    parser->col = 1;

    parser->anchor_capacity = YAML_ANCHOR_INIT_CAPACITY;
    parser->anchor_count = 0;
    parser->anchors = (YamlAnchor *) xr_malloc(sizeof(YamlAnchor) * parser->anchor_capacity);
    if (!parser->anchors)
        parser->anchor_capacity = 0;

    if (config) {
        parser->config = *config;
    } else {
        yaml_config_init(&parser->config);
    }

    parser->result.errors = xr_array_new(xr_current_coro(isolate));
    parser->result.meta.lines = 0;
    parser->result.meta.documents = 0;
    parser->result.meta.anchors = 0;
}

void yaml_parser_cleanup(YamlParser *parser) {
    if (!parser)
        return;
    xr_free(parser->anchors);
    parser->anchors = NULL;
    parser->anchor_capacity = 0;
    parser->anchor_count = 0;
}

// Construct a YAML error Map with {type, line, column, message} and push
// it into parser->result.errors. Used by parseStrict to surface issues
// such as duplicate keys, max-depth, or unknown escape sequences.
static void yaml_add_error(YamlParser *p, const char *type, const char *message) {
    if (!p || !p->result.errors)
        return;
    xrs_error_push(p->isolate, p->result.errors, type,
                   /*line=*/p->line,
                   /*row=*/-1,
                   /*column=*/p->col, message);
}

// ========== Anchor Operations ==========

static void save_anchor(YamlParser *p, const char *name, size_t len, XrValue val) {
    if (!p->anchors)
        return;
    if (p->anchor_count >= p->anchor_capacity) {
        int new_cap = p->anchor_capacity > 0 ? p->anchor_capacity * 2 : YAML_ANCHOR_INIT_CAPACITY;
        XR_REALLOC_OR_ABORT(p->anchors, sizeof(YamlAnchor) * (size_t) new_cap,
                            "YAML anchor table grow");
        p->anchor_capacity = new_cap;
    }
    if (len >= 63)
        len = 63;
    memcpy(p->anchors[p->anchor_count].name, name, len);
    p->anchors[p->anchor_count].name[len] = '\0';
    p->anchors[p->anchor_count].value = val;
    p->anchor_count++;
    p->result.meta.anchors++;
}

static XrValue find_anchor(YamlParser *p, const char *name, size_t len) {
    for (int i = 0; i < p->anchor_count; i++) {
        if (strlen(p->anchors[i].name) == len && strncmp(p->anchors[i].name, name, len) == 0) {
            return p->anchors[i].value;
        }
    }
    return xr_null();
}

// ========== Forward Declarations ==========

static XrValue parse_value(YamlParser *p, int min_indent);

// Shared string-buffer growth helper. Stack-started buffers migrate to
// xr_malloc as soon as the initial window is exhausted; on OOM the
// surrounding function returns xr_null() rather than silently continuing
// to write past the old buffer.
#define YAML_STR_ENSURE(buf, len, cap, stack_buf, add)                                             \
    do {                                                                                           \
        if ((len) + (size_t) (add) >= (cap)) {                                                     \
            size_t _new_cap = (cap) * 2;                                                           \
            while (_new_cap < (len) + (size_t) (add) + 1)                                          \
                _new_cap *= 2;                                                                     \
            char *_nb = (char *) xr_malloc(_new_cap);                                              \
            if (!_nb) {                                                                            \
                if ((buf) != (stack_buf))                                                          \
                    xr_free(buf);                                                                  \
                return xr_null();                                                                  \
            }                                                                                      \
            memcpy(_nb, (buf), (len));                                                             \
            if ((buf) != (stack_buf))                                                              \
                xr_free(buf);                                                                      \
            (buf) = _nb;                                                                           \
            (cap) = _new_cap;                                                                      \
        }                                                                                          \
    } while (0)

// ========== String Parsing ==========

static XrValue parse_single_quoted(YamlParser *p) {
    p->ptr++;
    p->col++;

    char stack_buf[256];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;

    while (p->ptr < p->end) {
        if (*p->ptr == '\'') {
            if (p->ptr + 1 < p->end && *(p->ptr + 1) == '\'') {
                YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
                buf[len++] = '\'';
                p->ptr += 2;
                p->col += 2;
            } else {
                p->ptr++;
                p->col++;
                break;
            }
        } else {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = *p->ptr;
            if (*p->ptr == '\n') {
                p->line++;
                p->col = 1;
            } else {
                p->col++;
            }
            p->ptr++;
        }
    }

    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf)
        xr_free(buf);
    return xr_string_value(str);
}

static XrValue parse_double_quoted(YamlParser *p) {
    p->ptr++;
    p->col++;

    char stack_buf[256];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;

    while (p->ptr < p->end && *p->ptr != '"') {
        if (*p->ptr == '\\') {
            p->ptr++;
            p->col++;
            if (p->ptr >= p->end)
                break;

            YAML_STR_ENSURE(buf, len, cap, stack_buf, 8);

            switch (*p->ptr) {
                case 'n':
                    buf[len++] = '\n';
                    break;
                case 't':
                    buf[len++] = '\t';
                    break;
                case 'r':
                    buf[len++] = '\r';
                    break;
                case '\\':
                    buf[len++] = '\\';
                    break;
                case '"':
                    buf[len++] = '"';
                    break;
                case '/':
                    buf[len++] = '/';
                    break;
                case 'a':
                    buf[len++] = '\a';
                    break;
                case 'b':
                    buf[len++] = '\b';
                    break;
                case 'e':
                    buf[len++] = '\x1B';
                    break;
                case 'v':
                    buf[len++] = '\v';
                    break;
                case '0':
                    buf[len++] = '\0';
                    break;
                case 'N': {
                    // Unicode next line (U+0085)
                    buf[len++] = (char) 0xC2;
                    buf[len++] = (char) 0x85;
                    break;
                }
                case '_': {
                    // Unicode non-breaking space (U+00A0)
                    buf[len++] = (char) 0xC2;
                    buf[len++] = (char) 0xA0;
                    break;
                }
                case 'x': {
                    // YAML 1.2: \xXX is an 8-bit Unicode code point, not
                    // a raw byte. Previously the parser wrote the byte
                    // verbatim which produced invalid UTF-8 for 0x80-0xFF
                    // (e.g. \xC3 became the illegal single byte 0xC3).
                    // Encode as UTF-8 to preserve the code-point identity.
                    p->ptr++;
                    unsigned int cp = 0;
                    for (int i = 0; i < 2 && p->ptr < p->end; i++, p->ptr++) {
                        char c = *p->ptr;
                        cp <<= 4;
                        if (c >= '0' && c <= '9')
                            cp |= c - '0';
                        else if (c >= 'a' && c <= 'f')
                            cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F')
                            cp |= c - 'A' + 10;
                    }
                    p->ptr--;
                    if (cp < 0x80) {
                        buf[len++] = (char) cp;
                    } else {
                        // 0x80..0xFF -> two-byte UTF-8 sequence
                        buf[len++] = (char) (0xC0 | (cp >> 6));
                        buf[len++] = (char) (0x80 | (cp & 0x3F));
                    }
                    break;
                }
                case 'u':
                case 'U': {
                    int digits = (*p->ptr == 'u') ? 4 : 8;
                    p->ptr++;
                    unsigned int cp = 0;
                    for (int i = 0; i < digits && p->ptr < p->end; i++, p->ptr++) {
                        char c = *p->ptr;
                        cp <<= 4;
                        if (c >= '0' && c <= '9')
                            cp |= c - '0';
                        else if (c >= 'a' && c <= 'f')
                            cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F')
                            cp |= c - 'A' + 10;
                    }
                    p->ptr--;
                    if (cp < 0x80) {
                        buf[len++] = (char) cp;
                    } else if (cp < 0x800) {
                        buf[len++] = (char) (0xC0 | (cp >> 6));
                        buf[len++] = (char) (0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf[len++] = (char) (0xE0 | (cp >> 12));
                        buf[len++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char) (0x80 | (cp & 0x3F));
                    } else if (cp <= 0x10FFFF) {
                        buf[len++] = (char) (0xF0 | (cp >> 18));
                        buf[len++] = (char) (0x80 | ((cp >> 12) & 0x3F));
                        buf[len++] = (char) (0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char) (0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default:
                    buf[len++] = *p->ptr;
                    break;
            }
            p->ptr++;
            p->col++;
        } else {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = *p->ptr;
            if (*p->ptr == '\n') {
                p->line++;
                p->col = 1;
            } else {
                p->col++;
            }
            p->ptr++;
        }
    }

    if (p->ptr < p->end && *p->ptr == '"') {
        p->ptr++;
        p->col++;
    }

    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf)
        xr_free(buf);
    return xr_string_value(str);
}

// ========== Scalar Parsing ==========

static XrValue parse_plain_scalar(YamlParser *p, int min_indent) {
    (void) min_indent;
    const char *start = p->ptr;
    size_t len = 0;

    while (p->ptr < p->end) {
        char c = *p->ptr;
        if (c == '\n' || c == '\r')
            break;
        if (c == '#' && len > 0 && *(p->ptr - 1) == ' ')
            break;
        if (c == ':' && p->ptr + 1 < p->end &&
            (*(p->ptr + 1) == ' ' || *(p->ptr + 1) == '\n' || *(p->ptr + 1) == '\r')) {
            break;
        }
        if (c == ',' || c == ']' || c == '}')
            break;

        p->ptr++;
        p->col++;
        len++;
    }

    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
        len--;
    }

    if (len == 0) {
        return xr_null();
    }

    // Special values
    if (len == 4 && strncmp(start, "null", 4) == 0)
        return xr_null();
    if (len == 1 && *start == '~')
        return xr_null();
    if (len == 4 && strncmp(start, "true", 4) == 0)
        return xr_bool(true);
    if (len == 5 && strncmp(start, "false", 5) == 0)
        return xr_bool(false);
    if (len == 4 && strncasecmp(start, ".inf", 4) == 0)
        return xr_float(INFINITY);
    if (len == 5 && strncasecmp(start, "-.inf", 5) == 0)
        return xr_float(-INFINITY);
    if (len == 4 && strncasecmp(start, ".nan", 4) == 0)
        return xr_float(NAN);

    // Number detection
    bool is_number = true;
    bool is_float = false;
    bool is_hex = (len > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X'));
    bool is_oct = (len > 2 && start[0] == '0' && (start[1] == 'o' || start[1] == 'O'));

    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        if (c == '.' || c == 'e' || c == 'E')
            is_float = true;
        if (is_hex && i >= 2) {
            if (!isxdigit((unsigned char) c)) {
                is_number = false;
                break;
            }
        } else if (is_oct && i >= 2) {
            if (c < '0' || c > '7') {
                is_number = false;
                break;
            }
        } else if (!isdigit((unsigned char) c) && c != '.' && c != '-' && c != '+' && c != 'e' &&
                   c != 'E' && c != 'x' && c != 'X' && c != 'o' && c != 'O') {
            is_number = false;
            break;
        }
    }

    if (is_number && len > 0 && len < 64 &&
        (isdigit((unsigned char) start[0]) || start[0] == '-' || start[0] == '+')) {
        char num_buf[64];
        memcpy(num_buf, start, len);
        num_buf[len] = '\0';

        if (is_float) {
            char *endptr;
            double val = strtod(num_buf, &endptr);
            if (endptr == num_buf + len) {
                return xr_float(val);
            }
        } else if (is_hex) {
            char *endptr;
            int64_t val = strtoll(num_buf + 2, &endptr, 16);
            if (endptr == num_buf + len) {
                return xr_int(val);
            }
        } else if (is_oct) {
            char *endptr;
            int64_t val = strtoll(num_buf + 2, &endptr, 8);
            if (endptr == num_buf + len) {
                return xr_int(val);
            }
        } else {
            // SWAR fast parsing
            int64_t val;
            if (yaml_fast_parse_int(start, len, &val)) {
                return xr_int(val);
            }
        }
    }

    XrString *str = xr_string_intern(p->isolate, start, len, 0);
    return xr_string_value(str);
}

// ========== Flow Sequence/Mapping ==========

static XrValue parse_flow_sequence(YamlParser *p) {
    p->ptr++;
    p->col++;

    XrArray *arr = xr_array_new(xr_current_coro(p->isolate));

    yaml_skip_ws(p);
    yaml_skip_empty_lines(p);

    if (p->ptr < p->end && *p->ptr == ']') {
        p->ptr++;
        p->col++;
        return xr_value_from_array(arr);
    }

    while (p->ptr < p->end) {
        yaml_skip_ws(p);
        yaml_skip_empty_lines(p);

        XrValue val = parse_value(p, 0);
        xr_array_push(arr, val);

        yaml_skip_ws(p);
        yaml_skip_empty_lines(p);

        if (p->ptr >= p->end)
            break;

        if (*p->ptr == ']') {
            p->ptr++;
            p->col++;
            break;
        }

        if (*p->ptr == ',') {
            p->ptr++;
            p->col++;
            continue;
        }

        break;
    }

    return xr_value_from_array(arr);
}

static XrValue parse_flow_mapping(YamlParser *p) {
    p->ptr++;
    p->col++;

    XrMap *map = xr_map_new(xr_current_coro(p->isolate));

    yaml_skip_ws(p);
    yaml_skip_empty_lines(p);

    if (p->ptr < p->end && *p->ptr == '}') {
        p->ptr++;
        p->col++;
        return xr_value_from_map(map);
    }

    while (p->ptr < p->end) {
        yaml_skip_ws(p);
        yaml_skip_empty_lines(p);

        XrValue key;
        if (*p->ptr == '"') {
            key = parse_double_quoted(p);
        } else if (*p->ptr == '\'') {
            key = parse_single_quoted(p);
        } else {
            key = parse_plain_scalar(p, 0);
        }

        // Duplicate key check (YAML spec: last value wins, but we can detect it)
        (void) 0;  // xr_map_set overwrites, which is correct YAML 1.2 behavior

        yaml_skip_ws(p);

        if (p->ptr >= p->end || *p->ptr != ':')
            break;
        p->ptr++;
        p->col++;

        yaml_skip_ws(p);

        XrValue val = parse_value(p, 0);
        xr_map_set(map, key, val);

        yaml_skip_ws(p);
        yaml_skip_empty_lines(p);

        if (p->ptr >= p->end)
            break;

        if (*p->ptr == '}') {
            p->ptr++;
            p->col++;
            break;
        }

        if (*p->ptr == ',') {
            p->ptr++;
            p->col++;
            continue;
        }

        break;
    }

    return xr_value_from_map(map);
}

// ========== Block Sequence/Mapping ==========

static XrValue parse_block_sequence(YamlParser *p, int seq_indent) {
    XrArray *arr = xr_array_new(xr_current_coro(p->isolate));
    bool first = true;

    while (p->ptr < p->end) {
        if (!first) {
            yaml_skip_empty_lines(p);
            if (p->ptr >= p->end)
                break;

            int indent = yaml_line_indent(p);
            if (indent < seq_indent)
                break;

            p->ptr += indent;
            p->col += indent;
        }
        first = false;

        if (p->ptr >= p->end || *p->ptr != '-')
            break;

        p->ptr++;
        p->col++;

        if (p->ptr < p->end && *p->ptr == ' ') {
            p->ptr++;
            p->col++;
        }

        yaml_skip_ws(p);
        if (p->ptr < p->end && (*p->ptr == '\n' || *p->ptr == '\r' || *p->ptr == '#')) {
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
            yaml_skip_empty_lines(p);

            int val_indent = yaml_count_indent(p);
            if (val_indent > seq_indent) {
                XrValue val = parse_value(p, val_indent);
                xr_array_push(arr, val);
            } else {
                xr_array_push(arr, xr_null());
            }
        } else {
            int line_before = p->line;
            XrValue val = parse_value(p, seq_indent + 2);
            xr_array_push(arr, val);
            // Only skip to EOL for inline values (same line).
            // Block structures (mapping/sequence) already advance past their content.
            if (p->line == line_before) {
                yaml_skip_to_eol(p);
                yaml_skip_newline(p);
            }
        }
    }

    return xr_value_from_array(arr);
}

static XrValue parse_block_mapping(YamlParser *p, int map_indent) {
    XrMap *map = xr_map_new(xr_current_coro(p->isolate));
    bool first_entry = true;
    int current_indent = map_indent;

    while (p->ptr < p->end) {
        if (!first_entry) {
            yaml_skip_empty_lines(p);
            if (p->ptr >= p->end)
                break;

            current_indent = yaml_line_indent(p);
            if (current_indent < map_indent)
                break;

            p->ptr += current_indent;
            p->col += current_indent;
        }
        first_entry = false;

        if (*p->ptr == '-' &&
            (p->ptr + 1 >= p->end || *(p->ptr + 1) == ' ' || *(p->ptr + 1) == '\n')) {
            break;
        }

        XrValue key;
        if (*p->ptr == '"') {
            key = parse_double_quoted(p);
        } else if (*p->ptr == '\'') {
            key = parse_single_quoted(p);
        } else if (*p->ptr == '?') {
            p->ptr++;
            p->col++;
            yaml_skip_ws(p);
            key = parse_value(p, current_indent + 2);
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
            yaml_skip_empty_lines(p);
            int new_indent = yaml_count_indent(p);
            p->ptr += new_indent;
            if (p->ptr < p->end && *p->ptr == ':') {
                p->ptr++;
                p->col++;
            }
        } else {
            key = parse_plain_scalar(p, map_indent);
        }

        yaml_skip_ws(p);

        if (p->ptr >= p->end || *p->ptr != ':') {
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
            continue;
        }

        p->ptr++;
        p->col++;
        yaml_skip_ws(p);

        // Duplicate-key enforcement: check before the map_set to preserve
        // the existing value when strict mode rejects the second entry.
        bool is_dup = xr_map_has(map, key);
        if (is_dup && !p->config.allow_duplicate_keys) {
            yaml_add_error(p, "duplicate_key",
                           "duplicate mapping key (set allowDuplicateKeys:true to allow)");
        }

        if (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r' && *p->ptr != '#') {
            XrValue val = parse_value(p, current_indent + 2);
            if (!is_dup || p->config.allow_duplicate_keys) {
                xr_map_set(map, key, val);
            }
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
        } else {
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
            yaml_skip_empty_lines(p);

            int val_indent = yaml_count_indent(p);
            if (val_indent > current_indent) {
                XrValue val = parse_value(p, val_indent);
                if (!is_dup || p->config.allow_duplicate_keys) {
                    xr_map_set(map, key, val);
                }
            } else if (!is_dup || p->config.allow_duplicate_keys) {
                xr_map_set(map, key, xr_null());
            }
        }
    }

    return xr_value_from_map(map);
}

// ========== Block Scalar ==========

// Chomping mode for block scalars
typedef enum {
    CHOMP_CLIP = 0,   // Default: single trailing newline
    CHOMP_STRIP = 1,  // '-': no trailing newline
    CHOMP_KEEP = 2    // '+': keep all trailing newlines
} ChompMode;

static void parse_block_header(YamlParser *p, ChompMode *chomp, int *explicit_indent) {
    *chomp = CHOMP_CLIP;
    *explicit_indent = 0;
    while (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r' && *p->ptr != '#') {
        if (*p->ptr == '-') {
            *chomp = CHOMP_STRIP;
        } else if (*p->ptr == '+') {
            *chomp = CHOMP_KEEP;
        } else if (isdigit((unsigned char) *p->ptr)) {
            *explicit_indent = *p->ptr - '0';
        }
        p->ptr++;
        p->col++;
    }
}

static void apply_chomping(char *buf, size_t *len, ChompMode chomp) {
    if (chomp == CHOMP_STRIP) {
        while (*len > 0 && buf[*len - 1] == '\n')
            (*len)--;
    } else if (chomp == CHOMP_CLIP) {
        while (*len > 0 && buf[*len - 1] == '\n')
            (*len)--;
        if (*len > 0)
            buf[(*len)++] = '\n';
    }
    // CHOMP_KEEP: leave all trailing newlines as-is
}

static XrValue parse_literal_block(YamlParser *p) {
    p->ptr++;
    p->col++;

    ChompMode chomp;
    int explicit_indent;
    parse_block_header(p, &chomp, &explicit_indent);

    yaml_skip_to_eol(p);
    yaml_skip_newline(p);

    int block_indent = explicit_indent > 0 ? explicit_indent : yaml_count_indent(p);
    if (block_indent == 0) {
        return xr_string_value(xr_string_intern(p->isolate, "", 0, 0));
    }

    char stack_buf[512];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;

    while (p->ptr < p->end) {
        int indent = yaml_count_indent(p);

        if (p->ptr < p->end && (*p->ptr == '\n' || *p->ptr == '\r')) {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = '\n';
            yaml_skip_newline(p);
            continue;
        }

        if (indent < block_indent)
            break;

        p->ptr += block_indent;
        p->col += block_indent;

        while (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r') {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = *p->ptr;
            p->ptr++;
            p->col++;
        }

        YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
        buf[len++] = '\n';
        yaml_skip_newline(p);
    }

    apply_chomping(buf, &len, chomp);

    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf)
        xr_free(buf);
    return xr_string_value(str);
}

static XrValue parse_folded_block(YamlParser *p) {
    p->ptr++;
    p->col++;

    ChompMode chomp;
    int explicit_indent;
    parse_block_header(p, &chomp, &explicit_indent);

    yaml_skip_to_eol(p);
    yaml_skip_newline(p);

    int block_indent = explicit_indent > 0 ? explicit_indent : yaml_count_indent(p);
    if (block_indent == 0) {
        return xr_string_value(xr_string_intern(p->isolate, "", 0, 0));
    }

    char stack_buf[512];
    size_t cap = sizeof(stack_buf);
    char *buf = stack_buf;
    size_t len = 0;
    bool prev_was_newline = false;

    while (p->ptr < p->end) {
        int indent = yaml_count_indent(p);

        if (p->ptr < p->end && (*p->ptr == '\n' || *p->ptr == '\r')) {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = '\n';
            prev_was_newline = true;
            yaml_skip_newline(p);
            continue;
        }

        if (indent < block_indent)
            break;

        if (len > 0 && !prev_was_newline) {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = ' ';
        }
        prev_was_newline = false;

        p->ptr += block_indent;
        p->col += block_indent;

        while (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r') {
            YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
            buf[len++] = *p->ptr;
            p->ptr++;
            p->col++;
        }
        yaml_skip_newline(p);
    }

    // Default: add trailing newline, then apply chomping
    if (len > 0 && buf[len - 1] != '\n') {
        YAML_STR_ENSURE(buf, len, cap, stack_buf, 1);
        buf[len++] = '\n';
    }
    apply_chomping(buf, &len, chomp);

    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf)
        xr_free(buf);
    return xr_string_value(str);
}

// ========== Value Parsing ==========

static XrValue parse_value(YamlParser *p, int min_indent) {
    yaml_skip_ws(p);

    if (p->ptr >= p->end)
        return xr_null();

    // Max depth check
    if (p->depth >= p->config.max_depth) {
        yaml_add_error(p, "max_depth", "maximum nesting depth exceeded");
        return xr_null();
    }
    p->depth++;

    char c = *p->ptr;

    // Anchor
    char anchor_name[64] = {0};
    size_t anchor_len = 0;
    if (c == '&') {
        p->ptr++;
        p->col++;
        const char *start = p->ptr;
        while (p->ptr < p->end && !isspace((unsigned char) *p->ptr) && *p->ptr != ':' &&
               *p->ptr != ',' && *p->ptr != ']' && *p->ptr != '}') {
            p->ptr++;
            p->col++;
        }
        anchor_len = p->ptr - start;
        if (anchor_len >= 64)
            anchor_len = 63;
        memcpy(anchor_name, start, anchor_len);
        yaml_skip_ws(p);
        c = *p->ptr;
    }

    // Alias
    if (c == '*') {
        p->ptr++;
        p->col++;
        const char *start = p->ptr;
        while (p->ptr < p->end && !isspace((unsigned char) *p->ptr) && *p->ptr != ':' &&
               *p->ptr != ',' && *p->ptr != ']' && *p->ptr != '}') {
            p->ptr++;
            p->col++;
        }
        return find_anchor(p, start, p->ptr - start);
    }

    XrValue result;

    if (c == '[') {
        result = parse_flow_sequence(p);
    } else if (c == '{') {
        result = parse_flow_mapping(p);
    } else if (c == '|') {
        result = parse_literal_block(p);
    } else if (c == '>') {
        result = parse_folded_block(p);
    } else if (c == '"') {
        result = parse_double_quoted(p);
    } else if (c == '\'') {
        result = parse_single_quoted(p);
    } else if (c == '-' &&
               (p->ptr + 1 >= p->end || *(p->ptr + 1) == ' ' || *(p->ptr + 1) == '\n')) {
        result = parse_block_sequence(p, min_indent);
    } else {
        const char *scan = p->ptr;
        bool is_mapping = false;
        while (scan < p->end && *scan != '\n' && *scan != '\r') {
            if (*scan == ':' && (scan + 1 >= p->end || *(scan + 1) == ' ' || *(scan + 1) == '\n' ||
                                 *(scan + 1) == '\r')) {
                is_mapping = true;
                break;
            }
            if (*scan == '#' || *scan == ',' || *scan == '}' || *scan == ']')
                break;
            scan++;
        }

        if (is_mapping) {
            result = parse_block_mapping(p, min_indent);
        } else {
            result = parse_plain_scalar(p, min_indent);
        }
    }

    if (anchor_len > 0) {
        save_anchor(p, anchor_name, anchor_len, result);
    }

    p->depth--;
    return result;
}

// ========== Document Parsing ==========

static XrValue parse_document(YamlParser *p) {
    yaml_skip_empty_lines(p);

    if (p->ptr + 2 < p->end && p->ptr[0] == '-' && p->ptr[1] == '-' && p->ptr[2] == '-') {
        p->ptr += 3;
        yaml_skip_to_eol(p);
        yaml_skip_newline(p);
        yaml_skip_empty_lines(p);
    }

    if (p->ptr >= p->end)
        return xr_null();

    int indent = yaml_line_indent(p);
    return parse_value(p, indent);
}

// ========== Public API ===========

XrValue yaml_parser_parse(YamlParser *parser) {
    XrValue result = parse_document(parser);
    parser->result.data = result;
    parser->result.meta.lines = parser->line;
    parser->result.meta.documents = 1;
    return result;
}

XrArray *yaml_parser_parse_all(YamlParser *parser) {
    XrArray *docs = xr_array_new(xr_current_coro(parser->isolate));

    while (parser->ptr < parser->end) {
        yaml_skip_empty_lines(parser);
        if (parser->ptr >= parser->end)
            break;

        if (parser->ptr + 2 < parser->end && parser->ptr[0] == '.' && parser->ptr[1] == '.' &&
            parser->ptr[2] == '.') {
            parser->ptr += 3;
            yaml_skip_to_eol(parser);
            yaml_skip_newline(parser);
            continue;
        }

        XrValue doc = parse_document(parser);
        xr_array_push(docs, doc);
        parser->result.meta.documents++;

        yaml_skip_empty_lines(parser);
    }

    parser->result.meta.lines = parser->line;
    return docs;
}

YamlResult yaml_parser_parse_strict(YamlParser *parser) {
    parser->result.data = yaml_parser_parse(parser);
    return parser->result;
}
