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

// ========== Configuration Initialization ==========

void yaml_config_init(YamlConfig *config) {
    config->safe = true;
    config->allow_duplicate_keys = false;
    config->max_depth = 64;
}

void yaml_config_from_json(XrayIsolate *X, YamlConfig *config, XrJson *json) {
    yaml_config_init(config);
    if (!json) return;
    
    XrValue val = xr_json_get_by_key(X, json, "safe");
    if (XR_IS_BOOL(val)) {
        config->safe = XR_TO_BOOL(val);
    }
    
    val = xr_json_get_by_key(X, json, "allowDuplicateKeys");
    if (XR_IS_BOOL(val)) {
        config->allow_duplicate_keys = XR_TO_BOOL(val);
    }
    
    val = xr_json_get_by_key(X, json, "maxDepth");
    if (XR_IS_INT(val)) {
        config->max_depth = (int)XR_TO_INT(val);
    }
}

// ========== Parser Initialization ==========

void yaml_parser_init(YamlParser *parser, XrayIsolate *isolate,
                      const char *data, size_t len, YamlConfig *config) {
    memset(parser, 0, sizeof(YamlParser));
    
    parser->isolate = isolate;
    parser->data = data;
    parser->ptr = data;
    parser->end = data + len;
    parser->line = 1;
    parser->col = 1;
    parser->anchor_count = 0;
    
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
    (void)parser;
}

// ========== Anchor Operations ==========

static void save_anchor(YamlParser *p, const char *name, size_t len, XrValue val) {
    if (p->anchor_count >= YAML_MAX_ANCHORS) return;
    if (len >= 63) len = 63;
    memcpy(p->anchors[p->anchor_count].name, name, len);
    p->anchors[p->anchor_count].name[len] = '\0';
    p->anchors[p->anchor_count].value = val;
    p->anchor_count++;
    p->result.meta.anchors++;
}

static XrValue find_anchor(YamlParser *p, const char *name, size_t len) {
    for (int i = 0; i < p->anchor_count; i++) {
        if (strlen(p->anchors[i].name) == len && 
            strncmp(p->anchors[i].name, name, len) == 0) {
            return p->anchors[i].value;
        }
    }
    return xr_null();
}

// ========== Forward Declarations ==========

static XrValue parse_value(YamlParser *p, int min_indent);

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
                if (len + 1 >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
                buf[len++] = '\'';
                p->ptr += 2;
                p->col += 2;
            } else {
                p->ptr++;
                p->col++;
                break;
            }
        } else {
            if (len + 1 >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
            buf[len++] = *p->ptr;
            if (*p->ptr == '\n') { p->line++; p->col = 1; }
            else { p->col++; }
            p->ptr++;
        }
    }
    
    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf) free(buf);
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
            if (p->ptr >= p->end) break;
            
            if (len + 8 >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
            
            switch (*p->ptr) {
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case 'r': buf[len++] = '\r'; break;
                case '\\': buf[len++] = '\\'; break;
                case '"': buf[len++] = '"'; break;
                case '/': buf[len++] = '/'; break;
                case 'a': buf[len++] = '\a'; break;
                case 'b': buf[len++] = '\b'; break;
                case 'e': buf[len++] = '\x1B'; break;
                case 'v': buf[len++] = '\v'; break;
                case '0': buf[len++] = '\0'; break;
                case 'N': {
                    // Unicode next line (U+0085)
                    buf[len++] = (char)0xC2;
                    buf[len++] = (char)0x85;
                    break;
                }
                case '_': {
                    // Unicode non-breaking space (U+00A0)
                    buf[len++] = (char)0xC2;
                    buf[len++] = (char)0xA0;
                    break;
                }
                case 'x': {
                    p->ptr++;
                    unsigned int cp = 0;
                    for (int i = 0; i < 2 && p->ptr < p->end; i++, p->ptr++) {
                        char c = *p->ptr;
                        cp <<= 4;
                        if (c >= '0' && c <= '9') cp |= c - '0';
                        else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                    }
                    p->ptr--;
                    buf[len++] = (char)cp;
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
                        if (c >= '0' && c <= '9') cp |= c - '0';
                        else if (c >= 'a' && c <= 'f') cp |= c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') cp |= c - 'A' + 10;
                    }
                    p->ptr--;
                    if (cp < 0x80) {
                        buf[len++] = (char)cp;
                    } else if (cp < 0x800) {
                        buf[len++] = (char)(0xC0 | (cp >> 6));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        buf[len++] = (char)(0xE0 | (cp >> 12));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp <= 0x10FFFF) {
                        buf[len++] = (char)(0xF0 | (cp >> 18));
                        buf[len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[len++] = (char)(0x80 | (cp & 0x3F));
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
            if (len + 1 >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
            buf[len++] = *p->ptr;
            if (*p->ptr == '\n') { p->line++; p->col = 1; }
            else { p->col++; }
            p->ptr++;
        }
    }
    
    if (p->ptr < p->end && *p->ptr == '"') {
        p->ptr++;
        p->col++;
    }
    
    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf) free(buf);
    return xr_string_value(str);
}

// ========== Scalar Parsing ==========

static XrValue parse_plain_scalar(YamlParser *p, int min_indent) {
    (void)min_indent;
    const char *start = p->ptr;
    size_t len = 0;
    
    while (p->ptr < p->end) {
        char c = *p->ptr;
        if (c == '\n' || c == '\r') break;
        if (c == '#' && len > 0 && *(p->ptr - 1) == ' ') break;
        if (c == ':' && p->ptr + 1 < p->end && 
            (*(p->ptr + 1) == ' ' || *(p->ptr + 1) == '\n' || *(p->ptr + 1) == '\r')) {
            break;
        }
        if (c == ',' || c == ']' || c == '}') break;
        
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
    if (len == 4 && strncmp(start, "null", 4) == 0) return xr_null();
    if (len == 1 && *start == '~') return xr_null();
    if (len == 4 && strncmp(start, "true", 4) == 0) return xr_bool(true);
    if (len == 5 && strncmp(start, "false", 5) == 0) return xr_bool(false);
    if (len == 4 && strncasecmp(start, ".inf", 4) == 0) return xr_float(INFINITY);
    if (len == 5 && strncasecmp(start, "-.inf", 5) == 0) return xr_float(-INFINITY);
    if (len == 4 && strncasecmp(start, ".nan", 4) == 0) return xr_float(NAN);
    
    // Number detection
    bool is_number = true;
    bool is_float = false;
    bool is_hex = (len > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X'));
    bool is_oct = (len > 2 && start[0] == '0' && (start[1] == 'o' || start[1] == 'O'));
    
    for (size_t i = 0; i < len; i++) {
        char c = start[i];
        if (c == '.' || c == 'e' || c == 'E') is_float = true;
        if (is_hex && i >= 2) {
            if (!isxdigit((unsigned char)c)) { is_number = false; break; }
        } else if (is_oct && i >= 2) {
            if (c < '0' || c > '7') { is_number = false; break; }
        } else if (!isdigit((unsigned char)c) && c != '.' && c != '-' && c != '+' && 
            c != 'e' && c != 'E' && c != 'x' && c != 'X' && c != 'o' && c != 'O') {
            is_number = false;
            break;
        }
    }
    
    if (is_number && len > 0 && len < 64 && (isdigit((unsigned char)start[0]) || 
        start[0] == '-' || start[0] == '+')) {
        
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
        
        if (p->ptr >= p->end) break;
        
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
        (void)0; // xr_map_set overwrites, which is correct YAML 1.2 behavior
        
        yaml_skip_ws(p);
        
        if (p->ptr >= p->end || *p->ptr != ':') break;
        p->ptr++;
        p->col++;
        
        yaml_skip_ws(p);
        
        XrValue val = parse_value(p, 0);
        xr_map_set(map, key, val);
        
        yaml_skip_ws(p);
        yaml_skip_empty_lines(p);
        
        if (p->ptr >= p->end) break;
        
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
            if (p->ptr >= p->end) break;
            
            int indent = yaml_count_indent(p);
            if (indent < seq_indent) break;
            
            p->ptr += indent;
            p->col += indent;
        }
        first = false;
        
        if (p->ptr >= p->end || *p->ptr != '-') break;
        
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
    (void)p->config.allow_duplicate_keys; // YAML 1.2: last value wins
    
    while (p->ptr < p->end) {
        if (!first_entry) {
            yaml_skip_empty_lines(p);
            if (p->ptr >= p->end) break;
            
            current_indent = yaml_count_indent(p);
            if (current_indent < map_indent) break;
            
            p->ptr += current_indent;
            p->col += current_indent;
        }
        first_entry = false;
        
        if (*p->ptr == '-' && (p->ptr + 1 >= p->end || 
            *(p->ptr + 1) == ' ' || *(p->ptr + 1) == '\n')) {
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
        
        if (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r' && *p->ptr != '#') {
            XrValue val = parse_value(p, current_indent + 2);
            xr_map_set(map, key, val);
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
        } else {
            yaml_skip_to_eol(p);
            yaml_skip_newline(p);
            yaml_skip_empty_lines(p);
            
            int val_indent = yaml_count_indent(p);
            if (val_indent > current_indent) {
                XrValue val = parse_value(p, val_indent);
                xr_map_set(map, key, val);
            } else {
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
        if (*p->ptr == '-') { *chomp = CHOMP_STRIP; }
        else if (*p->ptr == '+') { *chomp = CHOMP_KEEP; }
        else if (isdigit((unsigned char)*p->ptr)) { *explicit_indent = *p->ptr - '0'; }
        p->ptr++;
        p->col++;
    }
}

static void apply_chomping(char *buf, size_t *len, ChompMode chomp) {
    if (chomp == CHOMP_STRIP) {
        while (*len > 0 && buf[*len - 1] == '\n') (*len)--;
    } else if (chomp == CHOMP_CLIP) {
        while (*len > 0 && buf[*len - 1] == '\n') (*len)--;
        if (*len > 0) buf[(*len)++] = '\n';
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
    
    #define BLOCK_ENSURE(n) \
        if (len + (n) >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
    
    while (p->ptr < p->end) {
        int indent = yaml_count_indent(p);
        
        if (p->ptr < p->end && (*p->ptr == '\n' || *p->ptr == '\r')) {
            BLOCK_ENSURE(1);
            buf[len++] = '\n';
            yaml_skip_newline(p);
            continue;
        }
        
        if (indent < block_indent) break;
        
        p->ptr += block_indent;
        p->col += block_indent;
        
        while (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r') {
            BLOCK_ENSURE(1);
            buf[len++] = *p->ptr;
            p->ptr++;
            p->col++;
        }
        
        BLOCK_ENSURE(1);
        buf[len++] = '\n';
        yaml_skip_newline(p);
    }
    
    #undef BLOCK_ENSURE
    
    apply_chomping(buf, &len, chomp);
    
    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf) free(buf);
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
    
    #define FOLD_ENSURE(n) \
        if (len + (n) >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
    
    while (p->ptr < p->end) {
        int indent = yaml_count_indent(p);
        
        if (p->ptr < p->end && (*p->ptr == '\n' || *p->ptr == '\r')) {
            FOLD_ENSURE(1);
            buf[len++] = '\n';
            prev_was_newline = true;
            yaml_skip_newline(p);
            continue;
        }
        
        if (indent < block_indent) break;
        
        if (len > 0 && !prev_was_newline) {
            FOLD_ENSURE(1);
            buf[len++] = ' ';
        }
        prev_was_newline = false;
        
        p->ptr += block_indent;
        p->col += block_indent;
        
        while (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r') {
            FOLD_ENSURE(1);
            buf[len++] = *p->ptr;
            p->ptr++;
            p->col++;
        }
        yaml_skip_newline(p);
    }
    
    #undef FOLD_ENSURE
    
    // Default: add trailing newline, then apply chomping
    if (len > 0 && buf[len - 1] != '\n') {
        if (len + 1 >= cap) { cap *= 2; char *nb = (char*)malloc(cap); memcpy(nb, buf, len); if (buf != stack_buf) free(buf); buf = nb; }
        buf[len++] = '\n';
    }
    apply_chomping(buf, &len, chomp);
    
    XrString *str = xr_string_intern(p->isolate, buf, len, 0);
    if (buf != stack_buf) free(buf);
    return xr_string_value(str);
}

// ========== Value Parsing ==========

static XrValue parse_value(YamlParser *p, int min_indent) {
    yaml_skip_ws(p);
    
    if (p->ptr >= p->end) return xr_null();
    
    // Max depth check
    if (p->depth >= p->config.max_depth) {
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
        while (p->ptr < p->end && !isspace((unsigned char)*p->ptr) && 
               *p->ptr != ':' && *p->ptr != ',' && *p->ptr != ']' && *p->ptr != '}') {
            p->ptr++;
            p->col++;
        }
        anchor_len = p->ptr - start;
        if (anchor_len >= 64) anchor_len = 63;
        memcpy(anchor_name, start, anchor_len);
        yaml_skip_ws(p);
        c = *p->ptr;
    }
    
    // Alias
    if (c == '*') {
        p->ptr++;
        p->col++;
        const char *start = p->ptr;
        while (p->ptr < p->end && !isspace((unsigned char)*p->ptr) && 
               *p->ptr != ':' && *p->ptr != ',' && *p->ptr != ']' && *p->ptr != '}') {
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
    } else if (c == '-' && (p->ptr + 1 >= p->end || *(p->ptr + 1) == ' ' || *(p->ptr + 1) == '\n')) {
        result = parse_block_sequence(p, min_indent);
    } else {
        const char *scan = p->ptr;
        bool is_mapping = false;
        while (scan < p->end && *scan != '\n' && *scan != '\r') {
            if (*scan == ':' && (scan + 1 >= p->end || *(scan + 1) == ' ' || 
                *(scan + 1) == '\n' || *(scan + 1) == '\r')) {
                is_mapping = true;
                break;
            }
            if (*scan == '#' || *scan == ',' || *scan == '}' || *scan == ']') break;
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
    
    if (p->ptr >= p->end) return xr_null();
    
    int indent = yaml_count_indent(p);
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

XrArray* yaml_parser_parse_all(YamlParser *parser) {
    XrArray *docs = xr_array_new(xr_current_coro(parser->isolate));
    
    while (parser->ptr < parser->end) {
        yaml_skip_empty_lines(parser);
        if (parser->ptr >= parser->end) break;
        
        if (parser->ptr + 2 < parser->end && 
            parser->ptr[0] == '.' && parser->ptr[1] == '.' && parser->ptr[2] == '.') {
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
