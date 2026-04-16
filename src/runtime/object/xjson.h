/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.h - Structured data object with Hidden Class optimization
 *
 * KEY CONCEPT:
 *   - Inline fields[], O(1) access via Shape
 *   - Zero-copy shape transitions for property addition
 *   - Overflow PropertyArray for fields beyond in_object_capacity
 *   - No dictionary mode: Json is a pure data carrier
 */

#ifndef XJSON_H
#define XJSON_H

#include "../gc/xgc_header.h"
#include "../gc/xgc.h"
#include "../value/xvalue.h"
#include "xshape.h"
#include "xmap.h"
#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct XrJson XrJson;
struct XrCoroutine;

/* ========== XrJson Structure ========== */

/*
 * Overflow PropertyArray — heap-allocated spillover for fields
 * beyond in_object_capacity.  Allocated lazily on first overflow.
 *
 * Layout: [capacity(u16) | length(u16) | pad(4B)] [values[0] ... values[N-1]]
 */
typedef struct XrJsonOverflow {
    uint16_t capacity;
    uint16_t length;
    uint32_t _pad;
    XrValue values[];
} XrJsonOverflow;

struct XrJson {
    XrGCHeader gc;
    XrJsonOverflow *overflow;   // NULL when all fields fit in-object
    XrValue fields[];
};

// Retrieve shape via shape_id stored in GC header extra (replaces json->shape)
static inline XrShape* xr_json_shape(XrJson *json) {
    return xr_shape_get_by_id(xr_gc_get_shape_id(&json->gc));
}

// Set shape for a Json object (stores shape_id in GC header extra)
static inline void xr_json_set_shape(XrJson *json, XrShape *shape) {
    xr_gc_set_shape_id(&json->gc, shape->id);
}

/* ========== Creation API ========== */

XR_FUNC XrJson *xr_json_new(struct XrCoroutine *coro, uint16_t capacity);
XR_FUNC XrJson *xr_json_new_with_shape(struct XrCoroutine *coro, XrShape *shape);
XR_FUNC XrJson *xr_json_new_with_shape_noinit(struct XrCoroutine *coro, XrShape *shape);
XR_FUNC void xr_json_init_inplace(XrJson *json, XrShape *shape);
XR_FUNC size_t xr_json_size(int field_count);

/* ========== Field Access API ========== */

XR_FUNC XrValue xr_json_get(XrJson *json, SymbolId symbol);
XR_FUNC void xr_json_set(XrayIsolate *X, XrJson *json, SymbolId symbol, XrValue value);
XR_FUNC XrValue xr_json_get_by_key(XrayIsolate *X, XrJson *json, const char *key);
XR_FUNC void xr_json_set_by_key(XrayIsolate *X, XrJson *json, const char *key, XrValue value);

/* ========== Fast Path API (inline) ========== */

static inline XrValue xr_json_get_field(XrJson *json, uint16_t index) {
    return json->fields[index];
}

static inline void xr_json_set_field(XrJson *json, uint16_t index, XrValue value) {
    json->fields[index] = value;
}

// Access field by logical index (in-object or overflow)
static inline XrValue xr_json_get_field_any(XrJson *json, uint16_t index) {
    XrShape *shape = xr_json_shape(json);
    if (index < shape->in_object_capacity) {
        return json->fields[index];
    }
    XrJsonOverflow *ov = json->overflow;
    if (!ov) return xr_null();
    uint16_t ov_idx = index - shape->in_object_capacity;
    if (ov_idx >= ov->length) return xr_null();
    return ov->values[ov_idx];
}

/* ========== Query API ========== */

static inline uint16_t xr_json_field_count(XrJson *json) {
    if (!json) return 0;
    return xr_json_shape(json)->field_count;
}

static inline bool xr_json_has_field(XrJson *json, SymbolId symbol) {
    if (!json) return false;
    return xr_shape_has_field(xr_json_shape(json), symbol);
}


/* ========== GC Related ========== */

XR_FUNC void xr_gc_traverse_json(XrGC *gc, XrGCHeader *obj);

/* ========== XrValue Conversion ========== */

static inline XrValue xr_json_value(XrJson *json) {
    return XR_FROM_PTR(json);
}

static inline bool xr_value_is_json(XrValue v) {
    return XR_IS_PTR(v) && XR_GC_GET_TYPE((XrGCHeader*)XR_TO_PTR(v)) == XR_TJSON;
}

static inline XrJson* xr_value_to_json(XrValue v) {
    return (XrJson*)XR_TO_PTR(v);
}

#endif // XJSON_H
