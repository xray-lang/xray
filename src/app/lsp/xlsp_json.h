/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_json.h - Minimal JSON parser for LSP
 *
 * KEY CONCEPT:
 *   Lightweight JSON parser optimized for LSP message parsing.
 *   Uses a simple DOM-style representation.
 */

#ifndef XLSP_JSON_H
#define XLSP_JSON_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "../../base/xdefs.h"

// JSON value types
typedef enum {
    XR_JSON_NULL,
    XR_JSON_BOOL,
    XR_JSON_NUMBER,
    XR_JSON_STRING,
    XR_JSON_ARRAY,
    XR_JSON_OBJECT
} XrJsonType;

// Forward declaration
typedef struct XrJsonValue XrJsonValue;

// JSON object member
typedef struct XrJsonMember {
    char *key;
    XrJsonValue *value;
} XrJsonMember;

// JSON value
struct XrJsonValue {
    XrJsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            XrJsonValue **items;
            int count;
            int capacity;
        } array;
        struct {
            XrJsonMember *members;
            int count;
            int capacity;
        } object;
    } as;
};

// Parsing
XR_FUNC XrJsonValue *xlsp_json_parse(const char *json, size_t len);
XR_FUNC void xlsp_json_free(XrJsonValue *value);

// Deep clone a JSON value (caller owns the returned value)
XR_FUNC XrJsonValue *xlsp_json_clone(XrJsonValue *value);

// Object accessors
XR_FUNC XrJsonValue *xlsp_json_get(XrJsonValue *obj, const char *key);
XR_FUNC const char *xlsp_json_get_string(XrJsonValue *obj, const char *key);
XR_FUNC int64_t xlsp_json_get_int(XrJsonValue *obj, const char *key);
XR_FUNC int64_t xlsp_json_get_int_or(XrJsonValue *obj, const char *key, int64_t default_val);
XR_FUNC bool xlsp_json_get_bool(XrJsonValue *obj, const char *key);
XR_FUNC XrJsonValue *xlsp_json_get_array(XrJsonValue *obj, const char *key);
XR_FUNC XrJsonValue *xlsp_json_get_object(XrJsonValue *obj, const char *key);

// Array accessors
XR_FUNC int xlsp_json_array_len(XrJsonValue *arr);
XR_FUNC XrJsonValue *xlsp_json_array_get(XrJsonValue *arr, int index);

// Type checks
XR_FUNC bool xlsp_json_is_null(XrJsonValue *v);
XR_FUNC bool xlsp_json_is_string(XrJsonValue *v);
XR_FUNC bool xlsp_json_is_number(XrJsonValue *v);
XR_FUNC bool xlsp_json_is_bool(XrJsonValue *v);
XR_FUNC bool xlsp_json_is_array(XrJsonValue *v);
XR_FUNC bool xlsp_json_is_object(XrJsonValue *v);

// Building JSON
XR_FUNC XrJsonValue *xlsp_json_new_null(void);
XR_FUNC XrJsonValue *xlsp_json_new_bool(bool value);
XR_FUNC XrJsonValue *xlsp_json_new_number(double value);
XR_FUNC XrJsonValue *xlsp_json_new_string(const char *value);
XR_FUNC XrJsonValue *xlsp_json_new_array(void);
XR_FUNC XrJsonValue *xlsp_json_new_object(void);

XR_FUNC void xlsp_json_array_push(XrJsonValue *arr, XrJsonValue *value);
XR_FUNC void xlsp_json_object_set(XrJsonValue *obj, const char *key, XrJsonValue *value);

// Fast append: skip key dedup check (use when building new objects with unique keys)
XR_FUNC void xlsp_json_object_set_new(XrJsonValue *obj, const char *key, XrJsonValue *value);

// Serialization
XR_FUNC char *xlsp_json_stringify(XrJsonValue *value, size_t *out_len);

// Convenience macros for building JSON
#define XLSP_JSON_OBJ_START() xlsp_json_new_object()

#define XLSP_JSON_SET(obj, key, val) xlsp_json_object_set(obj, key, val)

#define XLSP_JSON_SET_STRING(obj, key, val) \
    xlsp_json_object_set(obj, key, xlsp_json_new_string(val))

#define XLSP_JSON_SET_INT(obj, key, val) \
    xlsp_json_object_set(obj, key, xlsp_json_new_number((double)(val)))

#define XLSP_JSON_SET_BOOL(obj, key, val) \
    xlsp_json_object_set(obj, key, xlsp_json_new_bool(val))

#define XLSP_JSON_SET_NULL(obj, key) \
    xlsp_json_object_set(obj, key, xlsp_json_new_null())

// Build a range object { start: {line, character}, end: {line, character} }
static inline XrJsonValue *xlsp_json_make_range(int start_line, int start_char,
                                                 int end_line, int end_char) {
    XrJsonValue *range = xlsp_json_new_object();
    XrJsonValue *start = xlsp_json_new_object();
    XrJsonValue *end = xlsp_json_new_object();
    XLSP_JSON_SET_INT(start, "line", start_line);
    XLSP_JSON_SET_INT(start, "character", start_char);
    XLSP_JSON_SET_INT(end, "line", end_line);
    XLSP_JSON_SET_INT(end, "character", end_char);
    XLSP_JSON_SET(range, "start", start);
    XLSP_JSON_SET(range, "end", end);
    return range;
}

// Build a position object { line, character }
static inline XrJsonValue *xlsp_json_make_position(int line, int character) {
    XrJsonValue *pos = xlsp_json_new_object();
    XLSP_JSON_SET_INT(pos, "line", line);
    XLSP_JSON_SET_INT(pos, "character", character);
    return pos;
}

// Build a location object { uri, range }
static inline XrJsonValue *xlsp_json_make_location(const char *uri,
                                                    int start_line, int start_char,
                                                    int end_line, int end_char) {
    XrJsonValue *loc = xlsp_json_new_object();
    XLSP_JSON_SET_STRING(loc, "uri", uri);
    XLSP_JSON_SET(loc, "range", xlsp_json_make_range(start_line, start_char, end_line, end_char));
    return loc;
}

#endif // XLSP_JSON_H
