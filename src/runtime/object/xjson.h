/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.h - Structured data object backed by dynamic-layout XrInstance
 *
 * KEY CONCEPT:
 *   - XrJson is a thin compatibility alias over XrInstance.
 *   - All Json objects use a dynamic-layout XrClass (V8-style hidden class).
 *   - Adding a property triggers a class transition; identical structures
 *     converge on the same descendant class.
 *   - In-object slots [0..capacity-2] hold inline values; slot [capacity-1]
 *     holds a heap pointer for overflow fields (auto-grown).
 */

#ifndef XJSON_H
#define XJSON_H

#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../class/xclass_system.h"
#include "../gc/xgc_header.h"
#include "../gc/xgc.h"
#include "../symbol/xsymbol_table.h"
#include "../value/xvalue.h"
#include "../xisolate_api.h"
#include <stdint.h>
#include <stdbool.h>

struct XrCoroutine;

/* ========== Type Alias ========== */

// Json objects are dynamic-layout XrInstance with class chains rooted at
// core->jsonRootClass. The XrJson alias is preserved so existing call
// sites keep compiling; new code should prefer XrInstance directly.
typedef XrInstance XrJson;

/* ========== Creation API ========== */

// Create an empty open-Json object on the running coroutine's heap.
XR_FUNC XrJson *xr_json_new(struct XrCoroutine *coro);

// Create a Json instance attached to a pre-built sealed/transition class.
// The class must be a dynamic-layout class (XR_CLASS_DYNAMIC_LAYOUT).
XR_FUNC XrJson *xr_json_new_with_class(struct XrCoroutine *coro, XrClass *cls);

// Initialize a Json in-place on pre-allocated memory (used by shared Json
// allocations on the system heap).
XR_FUNC void xr_json_init_inplace(XrJson *json, XrClass *cls);

// Byte size of a Json instance given its class.
XR_FUNC size_t xr_json_size(XrClass *cls);

/* ========== Field Access API ========== */

XR_FUNC XrValue xr_json_get(XrayIsolate *X, XrJson *json, SymbolId symbol);
XR_FUNC bool xr_json_set(XrayIsolate *X, XrJson *json, SymbolId symbol, XrValue value);
XR_FUNC XrValue xr_json_get_by_key(XrayIsolate *X, XrJson *json, const char *key);
XR_FUNC bool xr_json_set_by_key(XrayIsolate *X, XrJson *json, const char *key, XrValue value);

/* ========== Query API ========== */

static inline uint16_t xr_json_field_count(XrayIsolate *X, XrJson *json) {
    (void) X;
    if (!json || !json->klass)
        return 0;
    return json->klass->field_count;
}

static inline bool xr_json_has_field(XrayIsolate *X, XrJson *json, SymbolId symbol) {
    (void) X;
    if (!json || !json->klass)
        return false;
    return xr_class_lookup_field(json->klass, (int) symbol) >= 0;
}

/* ========== XrValue Conversion ========== */

static inline XrValue xr_json_value(XrJson *json) {
    return XR_FROM_PTR(json);
}

// A Json is any instance whose class carries XR_CLASS_JSON — this covers
// the root class and all hidden-class transitions derived from it.
static inline bool xr_value_is_json(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && (inst->klass->flags & XR_CLASS_JSON);
}

static inline XrJson *xr_value_to_json(XrValue v) {
    return (XrJson *) XR_TO_PTR(v);
}

#endif  // XJSON_H
