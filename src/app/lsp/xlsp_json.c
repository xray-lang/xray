/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_json.c - Minimal JSON parser implementation
 */

#include "xlsp_json.h"
#include "../../runtime/object/xutf8.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

// Parser state
typedef struct {
    const char *src;
    const char *end;
    const char *pos;
} JsonParser;

static void skip_whitespace(JsonParser *p) {
    while (p->pos < p->end && isspace((unsigned char)*p->pos)) {
        p->pos++;
    }
}

static XrJsonValue *parse_value(JsonParser *p);

static XrJsonValue *alloc_value(XrJsonType type) {
    XrJsonValue *v = xr_calloc(1, sizeof(XrJsonValue));
    if (v) v->type = type;
    return v;
}

static XrJsonValue *parse_null(JsonParser *p) {
    if (p->pos + 4 <= p->end && strncmp(p->pos, "null", 4) == 0) {
        p->pos += 4;
        return alloc_value(XR_JSON_NULL);
    }
    return NULL;
}

static XrJsonValue *parse_bool(JsonParser *p) {
    if (p->pos + 4 <= p->end && strncmp(p->pos, "true", 4) == 0) {
        p->pos += 4;
        XrJsonValue *v = alloc_value(XR_JSON_BOOL);
        if (v) v->as.boolean = true;
        return v;
    }
    if (p->pos + 5 <= p->end && strncmp(p->pos, "false", 5) == 0) {
        p->pos += 5;
        XrJsonValue *v = alloc_value(XR_JSON_BOOL);
        if (v) v->as.boolean = false;
        return v;
    }
    return NULL;
}

static XrJsonValue *parse_number(JsonParser *p) {
    const char *start = p->pos;
    
    if (*p->pos == '-') p->pos++;
    
    if (p->pos >= p->end || !isdigit((unsigned char)*p->pos)) {
        p->pos = start;
        return NULL;
    }
    
    while (p->pos < p->end && isdigit((unsigned char)*p->pos)) p->pos++;
    
    if (p->pos < p->end && *p->pos == '.') {
        p->pos++;
        while (p->pos < p->end && isdigit((unsigned char)*p->pos)) p->pos++;
    }
    
    if (p->pos < p->end && (*p->pos == 'e' || *p->pos == 'E')) {
        p->pos++;
        if (p->pos < p->end && (*p->pos == '+' || *p->pos == '-')) p->pos++;
        while (p->pos < p->end && isdigit((unsigned char)*p->pos)) p->pos++;
    }
    
    double value = strtod(start, NULL);
    
    XrJsonValue *v = alloc_value(XR_JSON_NUMBER);
    if (v) v->as.number = value;
    return v;
}

// Helper: parse 4 hex digits into a 16-bit value
static int parse_hex4(const char *s, uint16_t *out) {
    uint16_t val = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else return 0;  // invalid hex digit
    }
    *out = val;
    return 1;
}

// Helper: check if value is UTF-16 high surrogate (D800-DBFF)
static inline bool is_high_surrogate(uint16_t u) {
    return u >= 0xD800 && u <= 0xDBFF;
}

// Helper: check if value is UTF-16 low surrogate (DC00-DFFF)
static inline bool is_low_surrogate(uint16_t u) {
    return u >= 0xDC00 && u <= 0xDFFF;
}

// Helper: combine surrogate pair into codepoint
static inline uint32_t surrogate_to_codepoint(uint16_t high, uint16_t low) {
    return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
}

static char *parse_string_content(JsonParser *p) {
    if (p->pos >= p->end || *p->pos != '"') return NULL;
    p->pos++;  // skip opening quote
    
    // JSON decoded string is never longer than encoded form,
    // so input length is a safe upper bound (no first-pass needed)
    size_t max_len = (size_t)(p->end - p->pos);
    char *result = xr_malloc(max_len + 1);
    if (!result) return NULL;
    
    char *dst = result;
    while (p->pos < p->end && *p->pos != '"') {
        if (*p->pos == '\\' && p->pos + 1 < p->end) {
            p->pos++;  // skip backslash
            switch (*p->pos) {
                case '"':  *dst++ = '"'; p->pos++; break;
                case '\\': *dst++ = '\\'; p->pos++; break;
                case '/':  *dst++ = '/'; p->pos++; break;
                case 'b':  *dst++ = '\b'; p->pos++; break;
                case 'f':  *dst++ = '\f'; p->pos++; break;
                case 'n':  *dst++ = '\n'; p->pos++; break;
                case 'r':  *dst++ = '\r'; p->pos++; break;
                case 't':  *dst++ = '\t'; p->pos++; break;
                case 'u': {
                    // Parse \uXXXX unicode escape
                    p->pos++;  // skip 'u'
                    if (p->pos + 4 > p->end) {
                        // Not enough chars, copy as-is
                        *dst++ = 'u';
                        break;
                    }
                    
                    uint16_t u1;
                    if (!parse_hex4(p->pos, &u1)) {
                        // Invalid hex, copy as-is
                        *dst++ = 'u';
                        break;
                    }
                    p->pos += 4;
                    
                    uint32_t codepoint;
                    
                    // Check for UTF-16 surrogate pair
                    if (is_high_surrogate(u1)) {
                        // Expect \uDCxx low surrogate
                        if (p->pos + 6 <= p->end && 
                            p->pos[0] == '\\' && p->pos[1] == 'u') {
                            uint16_t u2;
                            if (parse_hex4(p->pos + 2, &u2) && is_low_surrogate(u2)) {
                                codepoint = surrogate_to_codepoint(u1, u2);
                                p->pos += 6;  // skip \uXXXX
                            } else {
                                // Invalid surrogate pair, use replacement char
                                codepoint = XR_UNICODE_INVALID;
                            }
                        } else {
                            // Missing low surrogate, use replacement char
                            codepoint = XR_UNICODE_INVALID;
                        }
                    } else if (is_low_surrogate(u1)) {
                        // Lone low surrogate, invalid
                        codepoint = XR_UNICODE_INVALID;
                    } else {
                        // BMP character, direct mapping
                        codepoint = u1;
                    }
                    
                    // Encode codepoint to UTF-8
                    int written = xr_utf8_encode(codepoint, dst);
                    if (written > 0) {
                        dst += written;
                    } else {
                        // Encoding failed, use replacement char
                        int w = xr_utf8_encode(XR_UNICODE_INVALID, dst);
                        if (w > 0) dst += w;
                    }
                    break;
                }
                default:
                    // Unknown escape, copy the character after backslash
                    *dst++ = *p->pos++;
                    break;
            }
        } else {
            *dst++ = *p->pos++;
        }
    }
    *dst = '\0';
    
    if (p->pos < p->end && *p->pos == '"') {
        p->pos++;  // skip closing quote
    }
    
    return result;
}

static XrJsonValue *parse_string(JsonParser *p) {
    char *str = parse_string_content(p);
    if (!str) return NULL;
    
    XrJsonValue *v = alloc_value(XR_JSON_STRING);
    if (v) {
        v->as.string = str;
    } else {
        xr_free(str);
    }
    return v;
}

static XrJsonValue *parse_array(JsonParser *p) {
    if (p->pos >= p->end || *p->pos != '[') return NULL;
    p->pos++;
    
    XrJsonValue *arr = alloc_value(XR_JSON_ARRAY);
    if (!arr) return NULL;
    
    arr->as.array.capacity = 8;
    arr->as.array.items = xr_malloc(arr->as.array.capacity * sizeof(XrJsonValue*));
    arr->as.array.count = 0;
    
    skip_whitespace(p);
    
    if (p->pos < p->end && *p->pos == ']') {
        p->pos++;
        return arr;
    }
    
    while (p->pos < p->end) {
        skip_whitespace(p);
        XrJsonValue *item = parse_value(p);
        if (!item) {
            xlsp_json_free(arr);
            return NULL;
        }
        
        if (arr->as.array.count >= arr->as.array.capacity) {
            arr->as.array.capacity *= 2;
            arr->as.array.items = xr_realloc(arr->as.array.items,
                arr->as.array.capacity * sizeof(XrJsonValue*));
        }
        arr->as.array.items[arr->as.array.count++] = item;
        
        skip_whitespace(p);
        if (p->pos < p->end && *p->pos == ',') {
            p->pos++;
        } else if (p->pos < p->end && *p->pos == ']') {
            p->pos++;
            break;
        } else {
            xlsp_json_free(arr);
            return NULL;
        }
    }
    
    return arr;
}

static XrJsonValue *parse_object(JsonParser *p) {
    if (p->pos >= p->end || *p->pos != '{') return NULL;
    p->pos++;
    
    XrJsonValue *obj = alloc_value(XR_JSON_OBJECT);
    if (!obj) return NULL;
    
    obj->as.object.capacity = 8;
    obj->as.object.members = xr_malloc(obj->as.object.capacity * sizeof(XrJsonMember));
    obj->as.object.count = 0;
    
    skip_whitespace(p);
    
    if (p->pos < p->end && *p->pos == '}') {
        p->pos++;
        return obj;
    }
    
    while (p->pos < p->end) {
        skip_whitespace(p);
        
        char *key = parse_string_content(p);
        if (!key) {
            xlsp_json_free(obj);
            return NULL;
        }
        
        skip_whitespace(p);
        if (p->pos >= p->end || *p->pos != ':') {
            xr_free(key);
            xlsp_json_free(obj);
            return NULL;
        }
        p->pos++;
        
        skip_whitespace(p);
        XrJsonValue *value = parse_value(p);
        if (!value) {
            xr_free(key);
            xlsp_json_free(obj);
            return NULL;
        }
        
        if (obj->as.object.count >= obj->as.object.capacity) {
            obj->as.object.capacity *= 2;
            obj->as.object.members = xr_realloc(obj->as.object.members,
                obj->as.object.capacity * sizeof(XrJsonMember));
        }
        obj->as.object.members[obj->as.object.count].key = key;
        obj->as.object.members[obj->as.object.count].value = value;
        obj->as.object.count++;
        
        skip_whitespace(p);
        if (p->pos < p->end && *p->pos == ',') {
            p->pos++;
        } else if (p->pos < p->end && *p->pos == '}') {
            p->pos++;
            break;
        } else {
            xlsp_json_free(obj);
            return NULL;
        }
    }
    
    return obj;
}

static XrJsonValue *parse_value(JsonParser *p) {
    skip_whitespace(p);
    if (p->pos >= p->end) return NULL;
    
    switch (*p->pos) {
        case 'n': return parse_null(p);
        case 't':
        case 'f': return parse_bool(p);
        case '"': return parse_string(p);
        case '[': return parse_array(p);
        case '{': return parse_object(p);
        default:
            if (*p->pos == '-' || isdigit((unsigned char)*p->pos)) {
                return parse_number(p);
            }
            return NULL;
    }
}

XrJsonValue *xlsp_json_parse(const char *json, size_t len) {
    if (!json || len == 0) return NULL;
    
    JsonParser p = {
        .src = json,
        .end = json + len,
        .pos = json
    };
    
    return parse_value(&p);
}

void xlsp_json_free(XrJsonValue *value) {
    if (!value) return;
    
    switch (value->type) {
        case XR_JSON_STRING:
            xr_free(value->as.string);
            break;
        case XR_JSON_ARRAY:
            for (int i = 0; i < value->as.array.count; i++) {
                xlsp_json_free(value->as.array.items[i]);
            }
            xr_free(value->as.array.items);
            break;
        case XR_JSON_OBJECT:
            for (int i = 0; i < value->as.object.count; i++) {
                xr_free(value->as.object.members[i].key);
                xlsp_json_free(value->as.object.members[i].value);
            }
            xr_free(value->as.object.members);
            break;
        default:
            break;
    }
    
    xr_free(value);
}

XrJsonValue *xlsp_json_clone(XrJsonValue *value) {
    if (!value) return NULL;
    
    switch (value->type) {
        case XR_JSON_NULL:
            return xlsp_json_new_null();
            
        case XR_JSON_BOOL:
            return xlsp_json_new_bool(value->as.boolean);
            
        case XR_JSON_NUMBER:
            return xlsp_json_new_number(value->as.number);
            
        case XR_JSON_STRING:
            return xlsp_json_new_string(value->as.string);
            
        case XR_JSON_ARRAY: {
            XrJsonValue *arr = xlsp_json_new_array();
            for (int i = 0; i < value->as.array.count; i++) {
                XrJsonValue *item_clone = xlsp_json_clone(value->as.array.items[i]);
                if (item_clone) {
                    xlsp_json_array_push(arr, item_clone);
                }
            }
            return arr;
        }
        
        case XR_JSON_OBJECT: {
            XrJsonValue *obj = xlsp_json_new_object();
            for (int i = 0; i < value->as.object.count; i++) {
                XrJsonValue *val_clone = xlsp_json_clone(value->as.object.members[i].value);
                if (val_clone) {
                    xlsp_json_object_set(obj, value->as.object.members[i].key, val_clone);
                }
            }
            return obj;
        }
        
        default:
            return NULL;
    }
}

XrJsonValue *xlsp_json_get(XrJsonValue *obj, const char *key) {
    if (!obj || obj->type != XR_JSON_OBJECT || !key) return NULL;
    
    for (int i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.members[i].key, key) == 0) {
            return obj->as.object.members[i].value;
        }
    }
    return NULL;
}

const char *xlsp_json_get_string(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xlsp_json_get(obj, key);
    if (v && v->type == XR_JSON_STRING) return v->as.string;
    return NULL;
}

int64_t xlsp_json_get_int(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xlsp_json_get(obj, key);
    if (v && v->type == XR_JSON_NUMBER) return (int64_t)v->as.number;
    return 0;
}

int64_t xlsp_json_get_int_or(XrJsonValue *obj, const char *key, int64_t default_val) {
    XrJsonValue *v = xlsp_json_get(obj, key);
    if (v && v->type == XR_JSON_NUMBER) return (int64_t)v->as.number;
    return default_val;
}

bool xlsp_json_get_bool(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xlsp_json_get(obj, key);
    if (v && v->type == XR_JSON_BOOL) return v->as.boolean;
    return false;
}

XrJsonValue *xlsp_json_get_array(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xlsp_json_get(obj, key);
    if (v && v->type == XR_JSON_ARRAY) return v;
    return NULL;
}

XrJsonValue *xlsp_json_get_object(XrJsonValue *obj, const char *key) {
    XrJsonValue *v = xlsp_json_get(obj, key);
    if (v && v->type == XR_JSON_OBJECT) return v;
    return NULL;
}

int xlsp_json_array_len(XrJsonValue *arr) {
    if (!arr || arr->type != XR_JSON_ARRAY) return 0;
    return arr->as.array.count;
}

XrJsonValue *xlsp_json_array_get(XrJsonValue *arr, int index) {
    if (!arr || arr->type != XR_JSON_ARRAY) return NULL;
    if (index < 0 || index >= arr->as.array.count) return NULL;
    return arr->as.array.items[index];
}

bool xlsp_json_is_null(XrJsonValue *v) {
    return v && v->type == XR_JSON_NULL;
}

bool xlsp_json_is_string(XrJsonValue *v) {
    return v && v->type == XR_JSON_STRING;
}

bool xlsp_json_is_number(XrJsonValue *v) {
    return v && v->type == XR_JSON_NUMBER;
}

bool xlsp_json_is_bool(XrJsonValue *v) {
    return v && v->type == XR_JSON_BOOL;
}

bool xlsp_json_is_array(XrJsonValue *v) {
    return v && v->type == XR_JSON_ARRAY;
}

bool xlsp_json_is_object(XrJsonValue *v) {
    return v && v->type == XR_JSON_OBJECT;
}

// Building JSON

XrJsonValue *xlsp_json_new_null(void) {
    return alloc_value(XR_JSON_NULL);
}

XrJsonValue *xlsp_json_new_bool(bool value) {
    XrJsonValue *v = alloc_value(XR_JSON_BOOL);
    if (v) v->as.boolean = value;
    return v;
}

XrJsonValue *xlsp_json_new_number(double value) {
    XrJsonValue *v = alloc_value(XR_JSON_NUMBER);
    if (v) v->as.number = value;
    return v;
}

XrJsonValue *xlsp_json_new_string(const char *value) {
    XrJsonValue *v = alloc_value(XR_JSON_STRING);
    if (v) v->as.string = xr_strdup(value ? value : "");
    return v;
}

XrJsonValue *xlsp_json_new_array(void) {
    XrJsonValue *v = alloc_value(XR_JSON_ARRAY);
    if (v) {
        v->as.array.capacity = 8;
        v->as.array.items = xr_malloc(8 * sizeof(XrJsonValue*));
        v->as.array.count = 0;
    }
    return v;
}

XrJsonValue *xlsp_json_new_object(void) {
    XrJsonValue *v = alloc_value(XR_JSON_OBJECT);
    if (v) {
        v->as.object.capacity = 8;
        v->as.object.members = xr_malloc(8 * sizeof(XrJsonMember));
        v->as.object.count = 0;
    }
    return v;
}

void xlsp_json_array_push(XrJsonValue *arr, XrJsonValue *value) {
    if (!arr || arr->type != XR_JSON_ARRAY || !value) return;
    
    if (arr->as.array.count >= arr->as.array.capacity) {
        arr->as.array.capacity *= 2;
        arr->as.array.items = xr_realloc(arr->as.array.items,
            arr->as.array.capacity * sizeof(XrJsonValue*));
    }
    arr->as.array.items[arr->as.array.count++] = value;
}

void xlsp_json_object_set(XrJsonValue *obj, const char *key, XrJsonValue *value) {
    if (!obj || obj->type != XR_JSON_OBJECT || !key || !value) return;
    
    // Check if key exists
    for (int i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.members[i].key, key) == 0) {
            xlsp_json_free(obj->as.object.members[i].value);
            obj->as.object.members[i].value = value;
            return;
        }
    }
    
    // Add new member
    if (obj->as.object.count >= obj->as.object.capacity) {
        obj->as.object.capacity *= 2;
        obj->as.object.members = xr_realloc(obj->as.object.members,
            obj->as.object.capacity * sizeof(XrJsonMember));
    }
    obj->as.object.members[obj->as.object.count].key = xr_strdup(key);
    obj->as.object.members[obj->as.object.count].value = value;
    obj->as.object.count++;
}

void xlsp_json_object_set_new(XrJsonValue *obj, const char *key, XrJsonValue *value) {
    if (!obj || obj->type != XR_JSON_OBJECT || !key || !value) return;
    
    // Direct append without checking for existing key
    if (obj->as.object.count >= obj->as.object.capacity) {
        obj->as.object.capacity *= 2;
        obj->as.object.members = xr_realloc(obj->as.object.members,
            obj->as.object.capacity * sizeof(XrJsonMember));
    }
    obj->as.object.members[obj->as.object.count].key = xr_strdup(key);
    obj->as.object.members[obj->as.object.count].value = value;
    obj->as.object.count++;
}

// Stringify helpers
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StringBuilder;

static void sb_init(StringBuilder *sb) {
    sb->cap = 256;
    sb->buf = xr_malloc(sb->cap);
    sb->len = 0;
    sb->buf[0] = '\0';
}

static void sb_append(StringBuilder *sb, const char *str, size_t len) {
    if (sb->len + len + 1 > sb->cap) {
        while (sb->len + len + 1 > sb->cap) sb->cap *= 2;
        sb->buf = xr_realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, str, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

static void sb_append_str(StringBuilder *sb, const char *str) {
    sb_append(sb, str, strlen(str));
}

static void sb_append_char(StringBuilder *sb, char c) {
    sb_append(sb, &c, 1);
}

static void stringify_value(StringBuilder *sb, XrJsonValue *v);

static void stringify_string(StringBuilder *sb, const char *str) {
    sb_append_char(sb, '"');
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"':  sb_append_str(sb, "\\\""); break;
            case '\\': sb_append_str(sb, "\\\\"); break;
            case '\b': sb_append_str(sb, "\\b"); break;
            case '\f': sb_append_str(sb, "\\f"); break;
            case '\n': sb_append_str(sb, "\\n"); break;
            case '\r': sb_append_str(sb, "\\r"); break;
            case '\t': sb_append_str(sb, "\\t"); break;
            default:
                if ((unsigned char)*p < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
                    sb_append_str(sb, buf);
                } else {
                    sb_append_char(sb, *p);
                }
                break;
        }
    }
    sb_append_char(sb, '"');
}

static void stringify_value(StringBuilder *sb, XrJsonValue *v) {
    if (!v) {
        sb_append_str(sb, "null");
        return;
    }
    
    char buf[64];
    
    switch (v->type) {
        case XR_JSON_NULL:
            sb_append_str(sb, "null");
            break;
        case XR_JSON_BOOL:
            sb_append_str(sb, v->as.boolean ? "true" : "false");
            break;
        case XR_JSON_NUMBER:
            if (v->as.number == (int64_t)v->as.number) {
                snprintf(buf, sizeof(buf), "%lld", (long long)v->as.number);
            } else {
                snprintf(buf, sizeof(buf), "%g", v->as.number);
            }
            sb_append_str(sb, buf);
            break;
        case XR_JSON_STRING:
            stringify_string(sb, v->as.string);
            break;
        case XR_JSON_ARRAY:
            sb_append_char(sb, '[');
            for (int i = 0; i < v->as.array.count; i++) {
                if (i > 0) sb_append_char(sb, ',');
                stringify_value(sb, v->as.array.items[i]);
            }
            sb_append_char(sb, ']');
            break;
        case XR_JSON_OBJECT:
            sb_append_char(sb, '{');
            for (int i = 0; i < v->as.object.count; i++) {
                if (i > 0) sb_append_char(sb, ',');
                stringify_string(sb, v->as.object.members[i].key);
                sb_append_char(sb, ':');
                stringify_value(sb, v->as.object.members[i].value);
            }
            sb_append_char(sb, '}');
            break;
    }
}

char *xlsp_json_stringify(XrJsonValue *value, size_t *out_len) {
    StringBuilder sb;
    sb_init(&sb);
    stringify_value(&sb, value);
    if (out_len) *out_len = sb.len;
    return sb.buf;
}
