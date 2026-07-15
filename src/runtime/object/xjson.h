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
 *   - XrJson is the runtime handle name for Json-shaped XrInstance objects.
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
#include "../mem/xobj_header.h"
#include "../mem/xheap.h"
#include "../symbol/xsymbol_table.h"
#include "../value/xvalue.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include <stdint.h>
#include <stdbool.h>

struct XrCoroutine;

/* ========== Runtime Handle ========== */

// Json objects are dynamic-layout XrInstance with class chains rooted at
// core->jsonRootClass. XrJson names that runtime contract while sharing
// the same storage as XrInstance.
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

XR_FUNC XrValue xr_json_get(XrVMRuntime *X, XrJson *json, SymbolId symbol);
XR_FUNC bool xr_json_set(XrVMRuntime *X, XrJson *json, SymbolId symbol, XrValue value);
XR_FUNC XrValue xr_json_get_by_key(XrVMRuntime *X, XrJson *json, const char *key);
XR_FUNC bool xr_json_set_by_key(XrVMRuntime *X, XrJson *json, const char *key, XrValue value);

// Copy every field of `src` into `dst` (object spread `{...src}`). Each value
// is retained (src keeps its own reference); existing fields are overwritten so
// later spread parts override earlier ones. Returns false only if `dst` is
// sealed and a new field cannot be added.
XR_FUNC bool xr_json_merge(XrVMRuntime *X, XrJson *dst, XrJson *src);

/* ========== Query API ========== */

static inline uint16_t xr_json_field_count(XrVMRuntime *X, XrJson *json) {
    (void) X;
    if (!json || !json->klass)
        return 0;
    return json->klass->field_count;
}

static inline bool xr_json_has_field(XrVMRuntime *X, XrJson *json, SymbolId symbol) {
    (void) X;
    if (!json || !json->klass)
        return false;
    return xr_class_lookup_field(json->klass, (int) symbol) >= 0;
}

/* ========== XrValue Conversion ========== */

static inline XrValue xr_json_value(XrJson *json) {
    return XR_FROM_PTR(json);
}

// A Json is any instance whose class has builtin_kind == XR_BK_JSON — this
// covers the root class and all hidden-class transitions derived from it.
static inline bool xr_value_is_json(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_JSON;
}

static inline bool xr_value_is_record(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_RECORD;
}

static inline bool xr_value_has_object_shape(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass &&
           (inst->klass->builtin_kind == XR_BK_JSON || inst->klass->builtin_kind == XR_BK_RECORD);
}

static inline bool xr_json_value_matches_kind(XrValue value, uint8_t encoded_kind) {
    if (XR_IS_NULL(value))
        return xr_json_value_kind_base(encoded_kind) == XR_JSON_VALUE_NULL ||
               xr_json_value_kind_base(encoded_kind) == XR_JSON_VALUE_JSON ||
               xr_json_value_kind_is_nullable(encoded_kind);
    switch ((XrJsonValueKind) xr_json_value_kind_base(encoded_kind)) {
        case XR_JSON_VALUE_BOOL:
            return XR_IS_BOOL(value);
        case XR_JSON_VALUE_INT:
            return XR_IS_INT(value);
        case XR_JSON_VALUE_FLOAT:
            return XR_IS_FLOAT(value);
        case XR_JSON_VALUE_STRING:
            return XR_IS_STRING(value);
        case XR_JSON_VALUE_JSON:
            return XR_IS_BOOL(value) || XR_IS_INT(value) || XR_IS_FLOAT(value) ||
                   XR_IS_STRING(value) || XR_IS_ARRAY(value) || xr_value_is_json(value);
        case XR_JSON_VALUE_RECORD:
            return xr_value_has_object_shape(value);
        case XR_JSON_VALUE_ARRAY:
            return XR_IS_ARRAY(value);
        case XR_JSON_VALUE_NULL:
        case XR_JSON_VALUE_ANY:
        default:
            return false;
    }
}

static inline XrJson *xr_value_to_json(XrValue v) {
    return (XrJson *) XR_TO_PTR(v);
}

static inline XrValue xr_json_decode_record_with_class(XrVMRuntime *X, struct XrCoroutine *coro,
                                                       XrJson *src, XrClass *cls) {
    if (!X || !coro || !src || !cls || cls->field_count == 0 || !cls->fields)
        return xr_null();
    uint16_t field_count = cls->field_count;
    XrValue *decoded_values = (XrValue *) xr_malloc(sizeof(XrValue) * field_count);
    if (!decoded_values)
        return xr_null();
    for (uint16_t fi = 0; fi < field_count; fi++) {
        XrFieldDescriptor *field = &cls->fields[fi];
        const char *fname = field->name;
        if (!fname) {
            xr_free(decoded_values);
            return xr_null();
        }
        XrValue field_val = xr_json_get_by_key(X, src, fname);
        if (XR_IS_NULL(field_val) && !xr_json_has_field(X, src, field->symbol)) {
            xr_free(decoded_values);
            return xr_null();
        }
        if (!xr_json_value_matches_kind(field_val, field->json_value_kind)) {
            xr_free(decoded_values);
            return xr_null();
        }
        if (xr_json_value_kind_base(field->json_value_kind) == XR_JSON_VALUE_RECORD &&
            !XR_IS_NULL(field_val)) {
            if (!field->json_record_class) {
                xr_free(decoded_values);
                return xr_null();
            }
            field_val = xr_json_decode_record_with_class(X, coro, xr_value_to_json(field_val),
                                                         field->json_record_class);
            if (XR_IS_NULL(field_val)) {
                xr_free(decoded_values);
                return xr_null();
            }
        }
        decoded_values[fi] = field_val;
    }

    XrJson *result = xr_json_new_with_class(coro, cls);
    if (!result) {
        xr_free(decoded_values);
        return xr_null();
    }
    for (uint16_t fi = 0; fi < field_count; fi++)
        xr_instance_set_dynamic_field(X, result, fi, decoded_values[fi]);
    xr_free(decoded_values);
    return XR_FROM_PTR(result);
}

#endif  // XJSON_H
