/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml_emitter.c - YAML serialization output
 *
 * KEY CONCEPT:
 *   Supports flowLevel and lineWidth to control output format.
 */

#include "yaml_parser.h"
#include "../../src/runtime/symbol/xsymbol_table.h"
#include "../../src/base/xmalloc.h"
#include "../common_writer.h"
#include "../common_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// ========== Serialization Configuration ==========

typedef struct {
    int indent;      // Indentation spaces, default 2
    int flow_level;  // Depth > flow_level uses flow style, default -1 (disabled)

    // Intended maximum output line width in characters (default 80).
    //
    // NOTE: currently reserved — the emitter does not yet wrap flow
    // sequences / mappings nor fold long plain scalars. The parameter
    // is kept in the API and YamlEmitConfig struct so existing callers
    // continue to compile, and so we have a stable place to hook a
    // future implementation (track chars-since-last-newline in
    // `current_line_len`, then emit a newline + continuation indent
    // once the running count passes `line_width`).
    //
    // If you are relying on wrap behaviour, do not rely on this field
    // today; file an issue so we can prioritise the implementation.
    int line_width;
} YamlEmitConfig;

// ========== Output Buffer ==========

typedef struct {
    XrSerWriter sw;  // shared byte buffer (sw.data / sw.len / sw.cap)
    XrayIsolate *isolate;
    YamlEmitConfig config;
    int current_line_len;
} YamlEmitter;

static inline void emit_init(YamlEmitter *e, XrayIsolate *isolate, YamlEmitConfig *config) {
    xr_serw_init(&e->sw, 256);
    e->isolate = isolate;
    e->current_line_len = 0;

    if (config) {
        e->config = *config;
    } else {
        e->config.indent = 2;
        e->config.flow_level = -1;
        e->config.line_width = 80;
    }
}

static inline void emit_free(YamlEmitter *e) {
    xr_serw_free(&e->sw);
}

static inline void emit_append(YamlEmitter *e, const char *s, size_t n) {
    xr_serw_append(&e->sw, s, n);
}

static inline void emit_str(YamlEmitter *e, const char *s) {
    xr_serw_str(&e->sw, s);
}

static inline void emit_char(YamlEmitter *e, char c) {
    xr_serw_char(&e->sw, c);
}

static inline void emit_indent(YamlEmitter *e, int level) {
    xr_serw_indent(&e->sw, level, e->config.indent);
}

// ========== String Quote Detection ==========

// Check whether the content parses as a YAML 1.1 bool/null token
// (yes/no/on/off/y/n plus case variants). Many YAML readers still honour
// 1.1 semantics so we quote these aggressively to preserve intent when
// re-parsing with a 1.1-compatible implementation.
static bool looks_like_yaml11_bool_or_null(const char *s, size_t len) {
    // Fast check by length
    if (len == 1) {
        char c = s[0];
        return c == 'y' || c == 'Y' || c == 'n' || c == 'N';
    }
    if (len == 2) {
        return (s[0] == 'n' || s[0] == 'N') && (s[1] == 'o' || s[1] == 'O');  // no/No/NO
    }
    if (len == 3) {
        // yes / Yes / YES
        if ((s[0] == 'y' || s[0] == 'Y') && (s[1] == 'e' || s[1] == 'E') &&
            (s[2] == 's' || s[2] == 'S'))
            return true;
        // off / Off / OFF (3) handled below by length==3 branch? off is 3 chars
        if ((s[0] == 'o' || s[0] == 'O') && (s[1] == 'f' || s[1] == 'F') &&
            (s[2] == 'f' || s[2] == 'F'))
            return true;
    }
    if (len == 2 && (s[0] == 'o' || s[0] == 'O') && (s[1] == 'n' || s[1] == 'N'))
        return true;  // on / On / ON
    return false;
}

// Check whether a scalar reparses as a YAML number or special float (.inf/.nan).
// If so we must quote to keep it a string on round-trip.
static bool looks_like_yaml_number(const char *s, size_t len) {
    if (len == 0)
        return false;
    size_t i = 0;
    if (s[i] == '+' || s[i] == '-')
        i++;
    if (i >= len)
        return false;
    // .inf / .nan forms
    if (s[i] == '.') {
        // .inf / .Inf / .INF / .nan / .NaN / .NAN
        if (len - i == 4) {
            if ((s[i + 1] == 'i' || s[i + 1] == 'I') && (s[i + 2] == 'n' || s[i + 2] == 'N') &&
                (s[i + 3] == 'f' || s[i + 3] == 'F'))
                return true;
            if ((s[i + 1] == 'n' || s[i + 1] == 'N') && (s[i + 2] == 'a' || s[i + 2] == 'A') &&
                (s[i + 3] == 'n' || s[i + 3] == 'N'))
                return true;
        }
        // Fall through: ".5" is a number
    }
    bool has_digit = false, has_dot = false, has_exp = false;
    for (; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            has_digit = true;
            continue;
        }
        if (c == '.' && !has_dot && !has_exp) {
            has_dot = true;
            continue;
        }
        if ((c == 'e' || c == 'E') && has_digit && !has_exp) {
            has_exp = true;
            if (i + 1 < len && (s[i + 1] == '+' || s[i + 1] == '-'))
                i++;
            continue;
        }
        if (c == '_')
            continue;  // YAML allows _ as digit separator
        return false;
    }
    return has_digit;
}

static bool needs_quote(const char *s, size_t len) {
    if (len == 0)
        return true;

    // YAML 1.2 null / bool keywords
    if (len == 4 && strncmp(s, "null", 4) == 0)
        return true;
    if (len == 4 && strncmp(s, "true", 4) == 0)
        return true;
    if (len == 5 && strncmp(s, "false", 5) == 0)
        return true;
    if (len == 1 && *s == '~')
        return true;
    // True / TRUE / False / FALSE / Null / NULL variants
    if (len == 4 && (strncmp(s, "True", 4) == 0 || strncmp(s, "TRUE", 4) == 0 ||
                     strncmp(s, "Null", 4) == 0 || strncmp(s, "NULL", 4) == 0))
        return true;
    if (len == 5 && (strncmp(s, "False", 5) == 0 || strncmp(s, "FALSE", 5) == 0))
        return true;

    // Leading or trailing whitespace would be stripped by block scalar rules
    if (s[0] == ' ' || s[0] == '\t' || s[len - 1] == ' ' || s[len - 1] == '\t')
        return true;

    // YAML 1.1 truthy/falsy tokens still parsed as bool by many readers
    if (looks_like_yaml11_bool_or_null(s, len))
        return true;

    // Looks-like-a-number: must quote to preserve string type
    if (looks_like_yaml_number(s, len))
        return true;

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '#' || c == '[' || c == ']' || c == '{' || c == '}' || c == ',' || c == '&' ||
            c == '*' || c == '!' || c == '|' || c == '>' || c == '\'' || c == '"' || c == '%' ||
            c == '@' || c == '`' || c == '\n' || c == '\r' || c == '\t' ||
            (unsigned char) c < 0x20) {
            return true;
        }
        // ": " (colon followed by space) breaks block mapping
        if (c == ':' && (i + 1 >= len || s[i + 1] == ' ' || s[i + 1] == '\t' || i + 1 == len)) {
            return true;
        }
        // " #" (space then hash) starts a comment
        if (c == ' ' && i + 1 < len && s[i + 1] == '#')
            return true;
    }

    char first = s[0];
    if (first == '-' || first == '?' || first == ':' || first == ' ' || first == '\t' ||
        isdigit((unsigned char) first)) {
        return true;
    }

    return false;
}

// ========== Forward Declarations ==========

static void emit_value(YamlEmitter *e, XrValue val, int level, bool flow_mode);

// ========== String Output ==========

static void emit_string(YamlEmitter *e, XrString *str, int level) {
    if (needs_quote(str->data, str->length)) {
        bool has_newline = false;
        for (size_t i = 0; i < str->length; i++) {
            if (str->data[i] == '\n') {
                has_newline = true;
                break;
            }
        }

        // Use block literal | form when the string contains newlines.
        // The old heuristic also required length > 40 which was arbitrary;
        // presence of newline is the meaningful trigger (block scalar is
        // the only way to emit them without \n escapes).
        if (has_newline) {
            emit_str(e, "|\n");
            const char *line_start = str->data;
            const char *end = str->data + str->length;
            while (line_start < end) {
                const char *line_end = line_start;
                while (line_end < end && *line_end != '\n')
                    line_end++;
                emit_indent(e, level + 1);
                emit_append(e, line_start, line_end - line_start);
                emit_char(e, '\n');
                line_start = line_end + 1;
            }
        } else {
            emit_char(e, '"');
            for (size_t i = 0; i < str->length; i++) {
                char c = str->data[i];
                switch (c) {
                    case '"':
                        emit_str(e, "\\\"");
                        break;
                    case '\\':
                        emit_str(e, "\\\\");
                        break;
                    case '\n':
                        emit_str(e, "\\n");
                        break;
                    case '\r':
                        emit_str(e, "\\r");
                        break;
                    case '\t':
                        emit_str(e, "\\t");
                        break;
                    default:
                        emit_char(e, c);
                        break;
                }
            }
            emit_char(e, '"');
        }
    } else {
        emit_append(e, str->data, str->length);
    }
}

// ========== Array Output ==========

static void emit_array(YamlEmitter *e, XrArray *arr, int level, bool flow_mode) {
    if (arr->length == 0) {
        emit_str(e, "[]");
        return;
    }

    bool use_flow = flow_mode;
    if (!use_flow && e->config.flow_level >= 0 && level >= e->config.flow_level) {
        use_flow = true;
    }

    if (!use_flow) {
        bool simple = true;
        for (int i = 0; i < arr->length && i < 5; i++) {
            XrValue v = xr_array_get(arr, i);
            if (XR_IS_ARRAY(v) || XR_IS_MAP(v)) {
                simple = false;
                break;
            }
        }
        if (simple && arr->length <= 5) {
            use_flow = true;
        }
    }

    if (use_flow) {
        emit_char(e, '[');
        for (int i = 0; i < arr->length; i++) {
            if (i > 0)
                emit_str(e, ", ");
            emit_value(e, xr_array_get(arr, i), level, true);
        }
        emit_char(e, ']');
    } else {
        for (int i = 0; i < arr->length; i++) {
            if (i > 0)
                emit_indent(e, level);
            emit_str(e, "- ");
            XrValue v = xr_array_get(arr, i);
            if (XR_IS_ARRAY(v) || XR_IS_MAP(v)) {
                emit_char(e, '\n');
                emit_indent(e, level + 1);
                emit_value(e, v, level + 1, false);
            } else {
                emit_value(e, v, level + 1, false);
            }
            if (i < arr->length - 1)
                emit_char(e, '\n');
        }
    }
}

// ========== Key Output Helper ==========

static void emit_key_string(YamlEmitter *e, const char *data, size_t length) {
    if (needs_quote(data, length)) {
        emit_char(e, '"');
        for (size_t i = 0; i < length; i++) {
            char c = data[i];
            switch (c) {
                case '"':
                    emit_str(e, "\\\"");
                    break;
                case '\\':
                    emit_str(e, "\\\\");
                    break;
                case '\n':
                    emit_str(e, "\\n");
                    break;
                case '\r':
                    emit_str(e, "\\r");
                    break;
                case '\t':
                    emit_str(e, "\\t");
                    break;
                default:
                    emit_char(e, c);
                    break;
            }
        }
        emit_char(e, '"');
    } else {
        emit_append(e, data, length);
    }
}

// ========== Map Output ==========

static void emit_map(YamlEmitter *e, XrMap *map, int level, bool flow_mode) {
    if (map->count == 0) {
        emit_str(e, "{}");
        return;
    }

    bool use_flow = flow_mode;
    if (!use_flow && e->config.flow_level >= 0 && level >= e->config.flow_level) {
        use_flow = true;
    }

    uint32_t size = xr_map_sizenode(map);

    if (use_flow) {
        emit_char(e, '{');
        bool first = true;
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *node = xr_map_node(map, i);
            if (node->key_tt > 0) {
                if (!first)
                    emit_str(e, ", ");
                first = false;
                if (XR_IS_STRING(node->key)) {
                    XrString *key = XR_TO_STRING(node->key);
                    emit_key_string(e, key->data, key->length);
                } else {
                    char buf[32];
                    if (XR_IS_INT(node->key)) {
                        snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(node->key));
                        emit_str(e, buf);
                    }
                }
                emit_str(e, ": ");
                emit_value(e, node->value, level, true);
            }
        }
        emit_char(e, '}');
    } else {
        bool first = true;
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *node = xr_map_node(map, i);
            if (node->key_tt > 0) {
                if (!first) {
                    emit_char(e, '\n');
                    emit_indent(e, level);
                }
                first = false;

                if (XR_IS_STRING(node->key)) {
                    XrString *key = XR_TO_STRING(node->key);
                    emit_key_string(e, key->data, key->length);
                } else {
                    char buf[32];
                    if (XR_IS_INT(node->key)) {
                        snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(node->key));
                        emit_str(e, buf);
                    }
                }
                emit_str(e, ": ");

                XrValue v = node->value;
                if (XR_IS_ARRAY(v) || XR_IS_MAP(v) || xr_value_is_json(v)) {
                    emit_char(e, '\n');
                    emit_indent(e, level + 1);
                    emit_value(e, v, level + 1, false);
                } else {
                    emit_value(e, v, level + 1, false);
                }
            }
        }
    }
}

// ========== Json Output ==========

static void emit_json(YamlEmitter *e, XrJson *json, int level, bool flow_mode) {
    XrSymbolTable *symtab = (XrSymbolTable *) e->isolate->symbol_table;
    uint16_t count = xr_json_field_count(e->isolate, json);

    if (count == 0) {
        emit_str(e, "{}");
        return;
    }

    bool use_flow = flow_mode;
    if (!use_flow && e->config.flow_level >= 0 && level >= e->config.flow_level) {
        use_flow = true;
    }

    XrShape *shape = xr_json_shape(e->isolate, json);
    if (use_flow) {
        emit_char(e, '{');
        for (uint16_t i = 0; i < shape->field_count; i++) {
            if (i > 0)
                emit_str(e, ", ");
            SymbolId sym = shape->field_symbols[i];
            const char *name = xr_symbol_get_name_in_table(symtab, sym);
            if (name)
                emit_key_string(e, name, strlen(name));
            emit_str(e, ": ");
            XrValue v = xr_json_get_field_any(e->isolate, json, i);
            emit_value(e, v, level, true);
        }
        emit_char(e, '}');
    } else {
        for (uint16_t i = 0; i < shape->field_count; i++) {
            if (i > 0) {
                emit_char(e, '\n');
                emit_indent(e, level);
            }
            SymbolId sym = shape->field_symbols[i];
            const char *name = xr_symbol_get_name_in_table(symtab, sym);
            if (name)
                emit_key_string(e, name, strlen(name));
            emit_str(e, ": ");
            XrValue v = xr_json_get_field_any(e->isolate, json, i);
            if (XR_IS_ARRAY(v) || XR_IS_MAP(v) || xr_value_is_json(v)) {
                emit_char(e, '\n');
                emit_indent(e, level + 1);
                emit_value(e, v, level + 1, false);
            } else {
                emit_value(e, v, level + 1, false);
            }
        }
    }
}

// ========== Value Output ==========

static void emit_value(YamlEmitter *e, XrValue val, int level, bool flow_mode) {
    // Cycle / pathological-depth guard. YAML's emitter does not (yet)
    // track anchors / aliases for graph-shaped inputs, so a user Map
    // that references itself would otherwise recurse through
    // emit_map -> emit_value -> emit_map ... until the C stack
    // overflows. Instead emit a null at the cut-off: the output is
    // truncated but the process stays alive and the error is locally
    // observable. XR_STDLIB_MAX_DEPTH matches the parser default so a
    // document we just emitted is always re-parseable.
    if (level >= XR_STDLIB_MAX_DEPTH) {
        emit_str(e, "null");
        return;
    }
    if (XR_IS_NULL(val)) {
        emit_str(e, "null");
    } else if (XR_IS_BOOL(val)) {
        emit_str(e, XR_TO_BOOL(val) ? "true" : "false");
    } else if (XR_IS_INT(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long) XR_TO_INT(val));
        emit_str(e, buf);
    } else if (XR_IS_FLOAT(val)) {
        double d = XR_TO_FLOAT(val);
        if (isinf(d)) {
            emit_str(e, d > 0 ? ".inf" : "-.inf");
        } else if (isnan(d)) {
            emit_str(e, ".nan");
        } else {
            char buf[32];
            // %.17g guarantees IEEE 754 binary64 round-trip; lower precision
            // (e.g. %.15g) silently truncates scientific/metric payloads.
            snprintf(buf, sizeof(buf), "%.17g", d);
            emit_str(e, buf);
        }
    } else if (XR_IS_STRING(val)) {
        emit_string(e, XR_TO_STRING(val), level);
    } else if (XR_IS_ARRAY(val)) {
        emit_array(e, XR_TO_ARRAY(val), level, flow_mode);
    } else if (xr_value_is_json(val)) {
        emit_json(e, xr_value_to_json(val), level, flow_mode);
    } else if (XR_IS_MAP(val)) {
        emit_map(e, XR_TO_MAP(val), level, flow_mode);
    }
}

// ========== Public API ===========

XR_FUNCDEF XrValue yaml_emit(XrayIsolate *isolate, XrValue value, int indent, int flow_level,
                             int line_width) {
    YamlEmitConfig config = {.indent = indent > 0 ? indent : 2,
                             .flow_level = flow_level,
                             .line_width = line_width > 0 ? line_width : 80};

    YamlEmitter emitter;
    emit_init(&emitter, isolate, &config);

    emit_value(&emitter, value, 0, false);
    emit_char(&emitter, '\n');

    XrString *result = xr_string_intern(isolate, emitter.sw.data, emitter.sw.len, 0);
    emit_free(&emitter);
    return xr_string_value(result);
}
