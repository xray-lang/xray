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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// ========== Serialization Configuration ==========

typedef struct {
    int indent;           // Indentation spaces, default 2
    int flow_level;       // Depth > flow_level uses flow style, default -1 (disabled)
    int line_width;       // Line width limit, default 80
} YamlEmitConfig;

// ========== Output Buffer ==========

typedef struct {
    XrayIsolate *isolate;
    char *buf;
    size_t len;
    size_t cap;
    YamlEmitConfig config;
    int current_line_len;
} YamlEmitter;

static void emit_init(YamlEmitter *e, XrayIsolate *isolate, YamlEmitConfig *config) {
    e->isolate = isolate;
    e->cap = 256;
    e->buf = (char*)malloc(e->cap);
    e->len = 0;
    e->current_line_len = 0;
    
    if (config) {
        e->config = *config;
    } else {
        e->config.indent = 2;
        e->config.flow_level = -1;
        e->config.line_width = 80;
    }
}

static void emit_append(YamlEmitter *e, const char *s, size_t n) {
    while (e->len + n >= e->cap) {
        e->cap *= 2;
        e->buf = (char*)realloc(e->buf, e->cap);
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
}

static void emit_str(YamlEmitter *e, const char *s) {
    emit_append(e, s, strlen(s));
}

static void emit_char(YamlEmitter *e, char c) {
    emit_append(e, &c, 1);
}

static void emit_indent(YamlEmitter *e, int level) {
    int n = level * e->config.indent;
    if (n <= 0) return;
    while (e->len + n >= e->cap) {
        e->cap *= 2;
        e->buf = (char*)realloc(e->buf, e->cap);
    }
    memset(e->buf + e->len, ' ', n);
    e->len += n;
}

// ========== String Quote Detection ==========

static bool needs_quote(const char *s, size_t len) {
    if (len == 0) return true;
    
    if (len == 4 && strncmp(s, "null", 4) == 0) return true;
    if (len == 4 && strncmp(s, "true", 4) == 0) return true;
    if (len == 5 && strncmp(s, "false", 5) == 0) return true;
    if (len == 1 && *s == '~') return true;
    
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == ':' || c == '#' || c == '[' || c == ']' || 
            c == '{' || c == '}' || c == ',' || c == '&' || 
            c == '*' || c == '!' || c == '|' || c == '>' ||
            c == '\'' || c == '"' || c == '%' || c == '@' ||
            c == '`' || c == '\n' || c == '\r') {
            return true;
        }
    }
    
    char first = s[0];
    if (first == '-' || first == '?' || first == ':' || 
        first == ' ' || isdigit((unsigned char)first)) {
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
        
        if (has_newline && str->length > 40) {
            emit_str(e, "|\n");
            const char *line_start = str->data;
            const char *end = str->data + str->length;
            while (line_start < end) {
                const char *line_end = line_start;
                while (line_end < end && *line_end != '\n') line_end++;
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
                    case '"': emit_str(e, "\\\""); break;
                    case '\\': emit_str(e, "\\\\"); break;
                    case '\n': emit_str(e, "\\n"); break;
                    case '\r': emit_str(e, "\\r"); break;
                    case '\t': emit_str(e, "\\t"); break;
                    default: emit_char(e, c); break;
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
            if (i > 0) emit_str(e, ", ");
            emit_value(e, xr_array_get(arr, i), level, true);
        }
        emit_char(e, ']');
    } else {
        for (int i = 0; i < arr->length; i++) {
            if (i > 0) emit_indent(e, level);
            emit_str(e, "- ");
            XrValue v = xr_array_get(arr, i);
            if (XR_IS_ARRAY(v) || XR_IS_MAP(v)) {
                emit_char(e, '\n');
                emit_indent(e, level + 1);
                emit_value(e, v, level + 1, false);
            } else {
                emit_value(e, v, level + 1, false);
            }
            if (i < arr->length - 1) emit_char(e, '\n');
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
                case '"': emit_str(e, "\\\""); break;
                case '\\': emit_str(e, "\\\\"); break;
                case '\n': emit_str(e, "\\n"); break;
                case '\r': emit_str(e, "\\r"); break;
                case '\t': emit_str(e, "\\t"); break;
                default: emit_char(e, c); break;
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
                if (!first) emit_str(e, ", ");
                first = false;
                if (XR_IS_STRING(node->key)) {
                    XrString *key = XR_TO_STRING(node->key);
                    emit_key_string(e, key->data, key->length);
                } else {
                    char buf[32];
                    if (XR_IS_INT(node->key)) {
                        snprintf(buf, sizeof(buf), "%lld", (long long)XR_TO_INT(node->key));
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
                        snprintf(buf, sizeof(buf), "%lld", (long long)XR_TO_INT(node->key));
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
    XrSymbolTable *symtab = (XrSymbolTable*)e->isolate->symbol_table;
    uint16_t count = xr_json_field_count(json);
    
    if (count == 0) {
        emit_str(e, "{}");
        return;
    }
    
    bool use_flow = flow_mode;
    if (!use_flow && e->config.flow_level >= 0 && level >= e->config.flow_level) {
        use_flow = true;
    }
    
    XrShape *shape = xr_json_shape(json);
    if (use_flow) {
        emit_char(e, '{');
        for (uint16_t i = 0; i < shape->field_count; i++) {
            if (i > 0) emit_str(e, ", ");
            SymbolId sym = shape->field_symbols[i];
            const char *name = xr_symbol_get_name_in_table(symtab, sym);
            if (name) emit_key_string(e, name, strlen(name));
            emit_str(e, ": ");
            XrValue v = xr_json_get_field_any(json, i);
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
            if (name) emit_key_string(e, name, strlen(name));
            emit_str(e, ": ");
            XrValue v = xr_json_get_field_any(json, i);
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
    if (XR_IS_NULL(val)) {
        emit_str(e, "null");
    } else if (XR_IS_BOOL(val)) {
        emit_str(e, XR_TO_BOOL(val) ? "true" : "false");
    } else if (XR_IS_INT(val)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)XR_TO_INT(val));
        emit_str(e, buf);
    } else if (XR_IS_FLOAT(val)) {
        double d = XR_TO_FLOAT(val);
        if (isinf(d)) {
            emit_str(e, d > 0 ? ".inf" : "-.inf");
        } else if (isnan(d)) {
            emit_str(e, ".nan");
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.15g", d);
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

XrValue yaml_emit(XrayIsolate *isolate, XrValue value, int indent, int flow_level, int line_width) {
    YamlEmitConfig config = {
        .indent = indent > 0 ? indent : 2,
        .flow_level = flow_level,
        .line_width = line_width > 0 ? line_width : 80
    };
    
    YamlEmitter emitter;
    emit_init(&emitter, isolate, &config);
    
    emit_value(&emitter, value, 0, false);
    emit_char(&emitter, '\n');
    
    XrString *result = xr_string_intern(isolate, emitter.buf, emitter.len, 0);
    free(emitter.buf);
    return xr_string_value(result);
}
