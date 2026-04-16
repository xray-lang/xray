/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_builtins.c - Built-in type method definitions for LSP
 *
 * KEY CONCEPT:
 *   This module provides a thin wrapper over xanalyzer_builtins
 *   for LSP-specific output (JSON completions, hover text).
 */

#include "xlsp_builtins.h"
#include "../../frontend/analyzer/xanalyzer_builtins.h"
#include "../../runtime/value/xtype.h"
#include <string.h>
#include <stdio.h>

// ============================================================================
// XrType creation helpers (for type conversion)
// ============================================================================

static XrType *create_type_for_builtin(XlspBuiltinType type) {
    switch (type) {
        case XLSP_TYPE_INT:           return xr_type_new_int();
        case XLSP_TYPE_FLOAT:         return xr_type_new_float();
        case XLSP_TYPE_STRING:        return xr_type_new_string();
        case XLSP_TYPE_BOOL:          return xr_type_new_bool();
        case XLSP_TYPE_ARRAY:         return xr_type_new_array(xr_type_new_unknown());
        case XLSP_TYPE_MAP:           return xr_type_new_map(xr_type_new_unknown(), xr_type_new_unknown());
        case XLSP_TYPE_SET:           return xr_type_new_set(xr_type_new_unknown());
        case XLSP_TYPE_JSON:          return xr_type_new_json();
        case XLSP_TYPE_CHANNEL:       return xr_type_new_channel(xr_type_new_unknown());
        case XLSP_TYPE_REGEX:         return xr_type_new_regex();
        case XLSP_TYPE_BIGINT:        return xr_type_new_bigint();
        case XLSP_TYPE_STRINGBUILDER: return xr_type_new_stringbuilder();
        case XLSP_TYPE_EXCEPTION:     return xr_type_new_named_instance("Exception");
        case XLSP_TYPE_COROUTINE:     return xr_type_new_task(NULL);
        default:                      return NULL;
    }
}

// ============================================================================
// Type name → XlspBuiltinType resolution
// ============================================================================

XlspBuiltinType xlsp_builtin_type_from_name(const char *name) {
    if (!name) return XLSP_TYPE_UNKNOWN;
    if (strcmp(name, TYPE_NAME_STRING) == 0)        return XLSP_TYPE_STRING;
    if (strcmp(name, TYPE_NAME_ARRAY) == 0)         return XLSP_TYPE_ARRAY;
    if (strcmp(name, TYPE_NAME_MAP) == 0)           return XLSP_TYPE_MAP;
    if (strcmp(name, TYPE_NAME_SET) == 0)           return XLSP_TYPE_SET;
    if (strcmp(name, TYPE_NAME_CHANNEL) == 0)       return XLSP_TYPE_CHANNEL;
    if (strcmp(name, TYPE_NAME_INT) == 0)           return XLSP_TYPE_INT;
    if (strcmp(name, TYPE_NAME_FLOAT) == 0)         return XLSP_TYPE_FLOAT;
    if (strcmp(name, TYPE_NAME_BOOL) == 0)          return XLSP_TYPE_BOOL;
    if (strcmp(name, TYPE_NAME_JSON) == 0)          return XLSP_TYPE_JSON;
    if (strcmp(name, TYPE_NAME_BIGINT) == 0)        return XLSP_TYPE_BIGINT;
    if (strcmp(name, TYPE_NAME_STRINGBUILDER) == 0) return XLSP_TYPE_STRINGBUILDER;
    if (strcmp(name, TYPE_NAME_REGEX) == 0)         return XLSP_TYPE_REGEX;
    if (strcmp(name, TYPE_NAME_EXCEPTION) == 0)     return XLSP_TYPE_EXCEPTION;
    if (strcmp(name, TYPE_NAME_COROUTINE) == 0)     return XLSP_TYPE_COROUTINE;
    return XLSP_TYPE_UNKNOWN;
}

// ============================================================================
// Completion and hover
// ============================================================================

XrJsonValue *xlsp_builtin_get_completions(XlspBuiltinType type) {
    XrJsonValue *items = xlsp_json_new_array();
    
    XrType *xa_type = create_type_for_builtin(type);
    if (!xa_type) return items;
    
    const XaBuiltinMember *members = NULL;
    int count = xa_builtin_get_members_for_type(xa_type, &members);
    
    for (int i = 0; i < count; i++) {
        const XaBuiltinMember *m = &members[i];
        
        XrJsonValue *item = xlsp_json_new_object();
        xlsp_json_object_set(item, "label", xlsp_json_new_string(m->name));
        int kind = m->is_method ? XLSP_KIND_METHOD : XLSP_KIND_PROPERTY;
        xlsp_json_object_set(item, "kind", xlsp_json_new_number(kind));
        
        if (m->signature) {
            // Build full signature: name + signature
            char detail[256];
            snprintf(detail, sizeof(detail), "%s%s", m->name, m->signature);
            xlsp_json_object_set(item, "detail", xlsp_json_new_string(detail));
        }
        if (m->doc) {
            xlsp_json_object_set(item, "documentation", xlsp_json_new_string(m->doc));
        }
        xlsp_json_array_push(items, item);
    }
    
    return items;
}

const char *xlsp_builtin_get_hover(XlspBuiltinType type, const char *method_name,
                                    char *buf, size_t buf_size) {
    XrType *xa_type = create_type_for_builtin(type);
    if (!xa_type) return NULL;
    
    const char *type_name = xa_builtin_get_type_name(xa_type);
    const char *signature = xa_builtin_get_member_signature(xa_type, method_name);
    const char *doc = xa_builtin_get_member_doc(xa_type, method_name);
    
    if (!signature) return NULL;
    
    snprintf(buf, buf_size, "```xray\n%s.%s%s\n```\n\n%s",
             type_name ? type_name : "unknown",
             method_name,
             signature,
             doc ? doc : "");
    
    return buf;
}

XlspBuiltinType xlsp_infer_literal_type(const char *text) {
    if (!text || !*text) return XLSP_TYPE_UNKNOWN;
    
    // String literal: "..." or '...'
    if (text[0] == '"' || text[0] == '\'') {
        return XLSP_TYPE_STRING;
    }
    
    // Array literal: [...]
    if (text[0] == '[') {
        return XLSP_TYPE_ARRAY;
    }
    
    // Object/Map literal: {...}
    if (text[0] == '{') {
        return XLSP_TYPE_JSON;
    }
    
    // Number literals
    int has_dot = 0;
    int is_number = 1;
    for (const char *p = text; *p; p++) {
        if (*p == '.') has_dot = 1;
        else if (*p < '0' || *p > '9') {
            if (p == text && *p == '-') continue;  // Negative
            is_number = 0;
            break;
        }
    }
    if (is_number && *text) {
        return has_dot ? XLSP_TYPE_FLOAT : XLSP_TYPE_INT;
    }
    
    // Constructor calls
    if (strncmp(text, TYPE_NAME_ARRAY, 5) == 0) return XLSP_TYPE_ARRAY;
    if (strncmp(text, TYPE_NAME_MAP, 3) == 0) return XLSP_TYPE_MAP;
    if (strncmp(text, TYPE_NAME_SET, 3) == 0) return XLSP_TYPE_SET;
    if (strncmp(text, "Bytes", 5) == 0) return XLSP_TYPE_ARRAY;
    if (strncmp(text, TYPE_NAME_CHANNEL, 7) == 0) return XLSP_TYPE_CHANNEL;
    if (strncmp(text, TYPE_NAME_BIGINT, 6) == 0) return XLSP_TYPE_BIGINT;
    if (strncmp(text, TYPE_NAME_STRINGBUILDER, 13) == 0) return XLSP_TYPE_STRINGBUILDER;
    if (strncmp(text, TYPE_NAME_REGEX, 5) == 0) return XLSP_TYPE_REGEX;
    
    return XLSP_TYPE_UNKNOWN;
}
