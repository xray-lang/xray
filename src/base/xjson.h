/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.h - Pure C JSON parser, builder, and serializer (L0)
 *
 * KEY CONCEPT:
 *   RFC 8259-compliant JSON parser with DOM-style representation.
 *   Zero runtime dependencies — usable from any layer.
 *   Provides parse, build, query, and serialize functionality.
 *
 * NAMING:
 *   Type:     XrJsonValue, XrJsonType, XrJsonMember
 *   Function: xjson_*
 *   Macro:    XJSON_*
 *
 *   The xjson_ prefix avoids collision with runtime/object/xjson.h
 *   (xr_json_*) which operates on GC-managed XrJson objects.
 */

#ifndef XJSON_DOM_H
#define XJSON_DOM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "xdefs.h"

/* ========== Types ========== */

typedef enum {
    XR_JSON_NULL,
    XR_JSON_BOOL,
    XR_JSON_NUMBER,
    XR_JSON_STRING,
    XR_JSON_ARRAY,
    XR_JSON_OBJECT
} XrJsonType;

typedef struct XrJsonValue XrJsonValue;

typedef struct XrJsonMember {
    char *key;
    XrJsonValue *value;
} XrJsonMember;

struct XrJsonValue {
    XrJsonType type;
    bool is_integer; /* true when number was parsed without '.' or 'e/E' */
    union {
        bool boolean;
        double number;
        int64_t integer; /* valid when type==XR_JSON_NUMBER && is_integer */
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

/* ========== Limits ========== */

#define XJSON_MAX_DEPTH 256

/* ========== Parsing ========== */

XR_FUNC XrJsonValue *xjson_parse(const char *json, size_t len);
XR_FUNC void xjson_free(XrJsonValue *value);

/* ========== Deep Clone ========== */

XR_FUNC XrJsonValue *xjson_clone(XrJsonValue *value);

/* ========== Object Accessors ========== */

XR_FUNC XrJsonValue *xjson_get(XrJsonValue *obj, const char *key);
XR_FUNC const char *xjson_get_string(XrJsonValue *obj, const char *key);
XR_FUNC int64_t xjson_get_int(XrJsonValue *obj, const char *key);
XR_FUNC int64_t xjson_get_int_or(XrJsonValue *obj, const char *key, int64_t default_val);
XR_FUNC bool xjson_get_bool(XrJsonValue *obj, const char *key);
XR_FUNC XrJsonValue *xjson_get_array(XrJsonValue *obj, const char *key);
XR_FUNC XrJsonValue *xjson_get_object(XrJsonValue *obj, const char *key);

/* ========== Array Accessors ========== */

XR_FUNC int xjson_array_len(XrJsonValue *arr);
XR_FUNC XrJsonValue *xjson_array_get(XrJsonValue *arr, int index);

/* ========== Type Checks ========== */

XR_FUNC bool xjson_is_null(XrJsonValue *v);
XR_FUNC bool xjson_is_string(XrJsonValue *v);
XR_FUNC bool xjson_is_number(XrJsonValue *v);
XR_FUNC bool xjson_is_bool(XrJsonValue *v);
XR_FUNC bool xjson_is_array(XrJsonValue *v);
XR_FUNC bool xjson_is_object(XrJsonValue *v);

/* ========== Builder ========== */

XR_FUNC XrJsonValue *xjson_new_null(void);
XR_FUNC XrJsonValue *xjson_new_bool(bool value);
XR_FUNC XrJsonValue *xjson_new_number(double value);
XR_FUNC XrJsonValue *xjson_new_string(const char *value);
XR_FUNC XrJsonValue *xjson_new_array(void);
XR_FUNC XrJsonValue *xjson_new_object(void);

XR_FUNC void xjson_array_push(XrJsonValue *arr, XrJsonValue *value);
XR_FUNC void xjson_array_truncate(XrJsonValue *arr, int max_len);
XR_FUNC void xjson_object_set(XrJsonValue *obj, const char *key, XrJsonValue *value);

/* Fast append: skip key dedup check (use when building new objects with unique keys) */
XR_FUNC void xjson_object_set_new(XrJsonValue *obj, const char *key, XrJsonValue *value);

/* ========== Serialization ========== */

XR_FUNC char *xjson_stringify(XrJsonValue *value, size_t *out_len);

/* ========== Convenience Macros ========== */

#define XJSON_OBJ_START() xjson_new_object()

#define XJSON_SET(obj, key, val) xjson_object_set(obj, key, val)

#define XJSON_SET_STRING(obj, key, val) xjson_object_set(obj, key, xjson_new_string(val))

#define XJSON_SET_INT(obj, key, val) xjson_object_set(obj, key, xjson_new_number((double) (val)))

#define XJSON_SET_BOOL(obj, key, val) xjson_object_set(obj, key, xjson_new_bool(val))

#define XJSON_SET_NULL(obj, key) xjson_object_set(obj, key, xjson_new_null())

/* ========== Inline Helpers ========== */

/* Build a range object { start: {line, character}, end: {line, character} } */
static inline XrJsonValue *xjson_make_range(int start_line, int start_char, int end_line,
                                            int end_char) {
    XrJsonValue *range = xjson_new_object();
    XrJsonValue *start = xjson_new_object();
    XrJsonValue *end = xjson_new_object();
    XJSON_SET_INT(start, "line", start_line);
    XJSON_SET_INT(start, "character", start_char);
    XJSON_SET_INT(end, "line", end_line);
    XJSON_SET_INT(end, "character", end_char);
    XJSON_SET(range, "start", start);
    XJSON_SET(range, "end", end);
    return range;
}

/* Build a position object { line, character } */
static inline XrJsonValue *xjson_make_position(int line, int character) {
    XrJsonValue *pos = xjson_new_object();
    XJSON_SET_INT(pos, "line", line);
    XJSON_SET_INT(pos, "character", character);
    return pos;
}

/* Build a location object { uri, range } */
static inline XrJsonValue *xjson_make_location(const char *uri, int start_line, int start_char,
                                               int end_line, int end_char) {
    XrJsonValue *loc = xjson_new_object();
    XJSON_SET_STRING(loc, "uri", uri);
    XJSON_SET(loc, "range", xjson_make_range(start_line, start_char, end_line, end_char));
    return loc;
}

#endif  // XJSON_DOM_H
