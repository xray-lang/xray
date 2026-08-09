/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson.h - Exact structural object storage and JSON codec helpers
 *
 * KEY CONCEPT:
 *   - XrObjectInstance stores a compiler-known, sealed structural shape.
 *   - The hidden-class chain is used only to intern fixed layouts.
 *   - JSON objects use XrMap and never this representation.
 */

#ifndef XJSON_H
#define XJSON_H

#include "../class/xclass.h"
#include "../class/xclass_lookup.h"
#include "../class/xinstance.h"
#include "../class/xclass_system.h"
#include "../class/xenum.h"
#include "../mem/xobj_header.h"
#include "../mem/xheap.h"
#include "../mem/xalloc_unified.h"
#include "../symbol/xsymbol_table.h"
#include "../value/xvalue.h"
#include "../value/xtype_names.h"
#include "xarray.h"
#include "xmap.h"
#include "xstring.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

struct XrCoroutine;

/* ========== Runtime Handle ========== */

// Exact structural objects share the XrInstance storage layout.
typedef XrInstance XrObjectInstance;

/* ========== Creation API ========== */

// Create an exact structural object attached to a pre-built sealed shape class.
XR_FUNC XrObjectInstance *xr_object_instance_new_with_class(struct XrCoroutine *coro, XrClass *cls);

// Initialize an exact object in-place on pre-allocated memory.
XR_FUNC void xr_object_instance_init_inplace(XrObjectInstance *json, XrClass *cls);

// Byte size of an exact object instance given its class.
XR_FUNC size_t xr_object_instance_size(XrClass *cls);

/* ========== Field Access API ========== */

XR_FUNC XrValue xr_object_instance_get(XrVMRuntime *X, XrObjectInstance *json, SymbolId symbol);
XR_FUNC bool xr_object_instance_set(XrVMRuntime *X, XrObjectInstance *json, SymbolId symbol,
                                    XrValue value);
XR_FUNC XrValue xr_object_instance_get_by_key(XrVMRuntime *X, XrObjectInstance *json,
                                              const char *key);
XR_FUNC bool xr_object_instance_set_by_key(XrVMRuntime *X, XrObjectInstance *json, const char *key,
                                           XrValue value);

// Copy every field of `src` into `dst` (object spread `{...src}`). Each value
// is retained (src keeps its own reference); existing fields are overwritten so
// later spread parts override earlier ones. Returns false for shape mismatch.
XR_FUNC bool xr_object_instance_merge(XrVMRuntime *X, XrObjectInstance *dst, XrObjectInstance *src);

/* ========== Query API ========== */

static inline uint16_t xr_object_instance_field_count(XrVMRuntime *X, XrObjectInstance *json) {
    (void) X;
    if (!json || !json->klass)
        return 0;
    return json->klass->field_count;
}

static inline bool xr_object_instance_has_field(XrVMRuntime *X, XrObjectInstance *json,
                                                SymbolId symbol) {
    (void) X;
    if (!json || !json->klass)
        return false;
    return xr_class_lookup_field(json->klass, (int) symbol) >= 0;
}

/* ========== XrValue Conversion ========== */

static inline XrValue xr_object_instance_value(XrObjectInstance *json) {
    return XR_FROM_PTR(json);
}

// JSON.Object is exactly Map<string, JSON.Value>. Runtime membership therefore
// follows the Map tag and never a hidden-class instance provenance bit.
static inline bool xr_value_is_json(XrValue v) {
    return XR_IS_MAP(v);
}

static inline bool xr_value_is_struct_object(XrValue v) {
    if (!XR_IS_INSTANCE(v))
        return false;
    XrInstance *inst = (XrInstance *) XR_TO_PTR(v);
    return inst->klass && inst->klass->builtin_kind == XR_BK_STRUCT_OBJECT;
}

static inline bool xr_value_has_object_shape(XrValue v) {
    return xr_value_is_struct_object(v);
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
                   XR_IS_STRING(value) || XR_IS_ARRAY(value) || XR_IS_MAP(value);
        case XR_JSON_VALUE_STRUCT_OBJECT:
            return xr_value_has_object_shape(value);
        case XR_JSON_VALUE_ARRAY:
            return XR_IS_ARRAY(value);
        case XR_JSON_VALUE_MAP:
            return XR_IS_MAP(value);
        case XR_JSON_VALUE_ENUM:
            return xr_value_is_enum_aggregate(value);
        case XR_JSON_VALUE_CLASS_INSTANCE:
            return XR_IS_INSTANCE(value) && !xr_value_has_object_shape(value);
        case XR_JSON_VALUE_NULL:
        case XR_JSON_VALUE_ANY:
        default:
            return false;
    }
}

static inline XrObjectInstance *xr_value_to_object_instance(XrValue v) {
    return (XrObjectInstance *) XR_TO_PTR(v);
}

static inline void xr_json_decode_release_partial(XrValue *values, uint16_t count) {
    for (uint16_t i = 0; values && i < count; i++)
        xr_rc_release_value(xr_current_coro_heap(), values[i]);
}

static inline XrValue xr_json_decode_struct_object_with_class(XrVMRuntime *X,
                                                              struct XrCoroutine *coro, XrMap *src,
                                                              XrClass *cls,
                                                              bool ignore_unknown_fields);
static inline XrValue xr_json_decode_struct_instance_with_class(XrVMRuntime *X,
                                                                struct XrCoroutine *coro,
                                                                XrObjectInstance *src, XrClass *cls,
                                                                bool ignore_unknown_fields);

static inline uint8_t xr_json_decode_schema_tid(const XrJsonDecodeSchema *schema) {
    if (!schema)
        return 0;
    switch ((XrJsonValueKind) xr_json_value_kind_base(schema->value_kind)) {
        case XR_JSON_VALUE_BOOL:
            return XR_TID_BOOL;
        case XR_JSON_VALUE_INT:
            return XR_TID_INT;
        case XR_JSON_VALUE_FLOAT:
            return XR_TID_FLOAT;
        case XR_JSON_VALUE_STRING:
            return XR_TID_STRING;
        case XR_JSON_VALUE_JSON:
            return 0;
        case XR_JSON_VALUE_ARRAY:
            return XR_TID_ARRAY;
        case XR_JSON_VALUE_MAP:
            return XR_TID_MAP;
        default:
            return 0;
    }
}

/* Decode one borrowed Json-DOM value into an owned value matching schema. */
static inline bool xr_json_decode_value_with_schema(XrVMRuntime *X, struct XrCoroutine *coro,
                                                    XrValue source,
                                                    const XrJsonDecodeSchema *schema,
                                                    bool ignore_unknown_fields, XrValue *out) {
    if (!X || !schema || !out)
        return false;
    if (XR_IS_NULL(source)) {
        if (xr_json_value_kind_base(schema->value_kind) != XR_JSON_VALUE_NULL &&
            xr_json_value_kind_base(schema->value_kind) != XR_JSON_VALUE_JSON &&
            !xr_json_value_kind_is_nullable(schema->value_kind))
            return false;
        *out = xr_null();
        return true;
    }

    switch ((XrJsonValueKind) xr_json_value_kind_base(schema->value_kind)) {
        case XR_JSON_VALUE_STRUCT_OBJECT: {
            if (xr_value_has_object_shape(source) && schema->target_descriptor) {
                XrValue decoded = xr_json_decode_struct_instance_with_class(
                    X, coro, xr_value_to_object_instance(source),
                    (XrClass *) schema->target_descriptor, ignore_unknown_fields);
                if (XR_IS_NULL(decoded))
                    return false;
                *out = decoded;
                return true;
            }
            if (!XR_IS_MAP(source) || !schema->target_descriptor)
                return false;
            XrValue decoded = xr_json_decode_struct_object_with_class(
                X, coro, XR_TO_MAP(source), (XrClass *) schema->target_descriptor,
                ignore_unknown_fields);
            if (XR_IS_NULL(decoded))
                return false;
            *out = decoded;
            return true;
        }
        case XR_JSON_VALUE_CLASS_INSTANCE: {
            XrString *class_name = (XrString *) schema->target_descriptor;
            XrClass *target = class_name ? xr_class_lookup_by_name(X, class_name->data) : NULL;
            if (!target || (target->flags & XR_CLASS_DERIVE_JSON) == 0 || !XR_IS_MAP(source))
                return false;
            XrValue decoded = xr_json_decode_struct_object_with_class(
                X, coro, XR_TO_MAP(source), target, ignore_unknown_fields);
            if (XR_IS_NULL(decoded))
                return false;
            *out = decoded;
            return true;
        }
        case XR_JSON_VALUE_ARRAY: {
            if (!XR_IS_ARRAY(source) || !schema->child)
                return false;
            XrArray *src = XR_TO_ARRAY(source);
            XrArrayElemType storage = schema->storage_type < XR_ELEM_COUNT
                                          ? (XrArrayElemType) schema->storage_type
                                          : XR_ELEM_ANY;
            XrArray *dst = xr_array_with_capacity_typed(coro, src->length, storage);
            if (!dst)
                return false;
            for (int32_t i = 0; i < src->length; i++) {
                XrValue item = xr_null();
                if (!xr_json_decode_value_with_schema(X, coro, xr_array_get(src, i), schema->child,
                                                      ignore_unknown_fields, &item)) {
                    xr_rc_release_value(xr_current_coro_heap(), XR_FROM_PTR(dst));
                    return false;
                }
                xr_array_push(dst, item);
            }
            *out = XR_FROM_PTR(dst);
            return true;
        }
        case XR_JSON_VALUE_MAP: {
            if (!XR_IS_MAP(source) || !schema->child)
                return false;
            XrMap *src = XR_TO_MAP(source);
            uint32_t count = src ? src->count : 0;
            XrMap *dst = xr_map_with_capacity(coro, count);
            if (!dst)
                return false;
            dst->key_tid = XR_TID_STRING;
            dst->value_tid = xr_json_decode_schema_tid(schema->child);
            for (uint32_t i = 0; src && i < src->nentries; i++) {
                XrMapEntry *entry = &src->entries[i];
                if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                    continue;
                XrValue value = xr_null();
                if (!XR_IS_STRING(entry->key) ||
                    !xr_json_decode_value_with_schema(X, coro, entry->value, schema->child,
                                                      ignore_unknown_fields, &value)) {
                    xr_rc_release_value(xr_current_coro_heap(), XR_FROM_PTR(dst));
                    return false;
                }
                xr_rc_retain_value(entry->key);
                xr_map_set(dst, entry->key, value);
            }
            *out = XR_FROM_PTR(dst);
            return true;
        }
        case XR_JSON_VALUE_ENUM: {
            XrEnumType *target = (XrEnumType *) schema->target_descriptor;
            if (!XR_IS_STRING(source) || !target || xr_enum_type_has_payloads(target))
                return false;
            XrString *name = XR_TO_STRING(source);
            for (uint32_t i = 0; i < target->member_count; i++) {
                const char *candidate = xr_enum_type_member_name(target, i);
                if (!candidate || strlen(candidate) != name->length ||
                    memcmp(candidate, name->data, name->length) != 0)
                    continue;
                XrEnumAggregateValue *value = xr_enum_zero_payload_value(X, target, i);
                if (!value)
                    return false;
                *out = XR_FROM_PTR(value);
                return true;
            }
            return false;
        }
        default:
            if (!xr_json_value_matches_kind(source, schema->value_kind))
                return false;
            xr_rc_retain_value(source);
            *out = source;
            return true;
    }
}

static inline XrValue xr_json_decode_struct_instance_with_class(XrVMRuntime *X,
                                                                struct XrCoroutine *coro,
                                                                XrObjectInstance *src, XrClass *cls,
                                                                bool ignore_unknown_fields) {
    XrClass *source_class = src ? xr_instance_get_class(src) : NULL;
    int target_fields = cls ? xr_class_instance_field_count(cls) : 0;
    int source_fields = source_class ? xr_class_instance_field_count(source_class) : 0;
    if (!X || !src || !source_class || !cls || target_fields <= 0 || !cls->fields)
        return xr_null();
    if (!ignore_unknown_fields && source_fields > target_fields)
        return xr_null();
    XrValue *decoded_values = (XrValue *) xr_malloc(sizeof(XrValue) * (size_t) target_fields);
    if (!decoded_values)
        return xr_null();
    for (int fi = 0; fi < target_fields; fi++) {
        XrFieldDescriptor *field = &cls->fields[fi];
        int source_index = -1;
        for (int si = 0; field->name && si < source_fields; si++) {
            const char *source_name = source_class->fields[si].name;
            if (source_name && strcmp(source_name, field->name) == 0) {
                source_index = si;
                break;
            }
        }
        if (source_index < 0) {
            if (xr_json_value_kind_is_nullable(field->json_decode_schema.value_kind)) {
                decoded_values[fi] = xr_null();
                continue;
            }
            xr_json_decode_release_partial(decoded_values, (uint16_t) fi);
            xr_free(decoded_values);
            return xr_null();
        }
        XrValue decoded = xr_null();
        if (!xr_json_decode_value_with_schema(
                X, coro, xr_instance_get_dynamic_field(src, (uint16_t) source_index),
                &field->json_decode_schema, ignore_unknown_fields, &decoded)) {
            xr_json_decode_release_partial(decoded_values, (uint16_t) fi);
            xr_free(decoded_values);
            return xr_null();
        }
        decoded_values[fi] = decoded;
    }

    XrObjectInstance *result = xr_object_instance_new_with_class(coro, cls);
    if (!result) {
        xr_json_decode_release_partial(decoded_values, (uint16_t) target_fields);
        xr_free(decoded_values);
        return xr_null();
    }
    for (int fi = 0; fi < target_fields; fi++) {
        if (xr_instance_set_dynamic_field(X, result, (uint16_t) fi, decoded_values[fi]))
            continue;
        for (int remaining = fi; remaining < target_fields; remaining++)
            xr_rc_release_value(xr_current_coro_heap(), decoded_values[remaining]);
        xr_rc_release_value(xr_current_coro_heap(), XR_FROM_PTR(result));
        xr_free(decoded_values);
        return xr_null();
    }
    xr_free(decoded_values);
    return XR_FROM_PTR(result);
}

static inline XrValue xr_json_decode_struct_object_with_class(XrVMRuntime *X,
                                                              struct XrCoroutine *coro, XrMap *src,
                                                              XrClass *cls,
                                                              bool ignore_unknown_fields) {
    int instance_fields = cls ? xr_class_instance_field_count(cls) : 0;
    if (!X || !src || !cls || instance_fields <= 0 || !cls->fields)
        return xr_null();
    uint16_t field_count = (uint16_t) instance_fields;
    if (!ignore_unknown_fields && src->count > field_count)
        return xr_null();
    XrValue *decoded_values = (XrValue *) xr_malloc(sizeof(XrValue) * field_count);
    if (!decoded_values)
        return xr_null();
    for (uint16_t fi = 0; fi < field_count; fi++) {
        XrFieldDescriptor *field = &cls->fields[fi];
        const char *fname = field->name;
        if (!fname) {
            xr_json_decode_release_partial(decoded_values, fi);
            xr_free(decoded_values);
            return xr_null();
        }
        XrString *key = xr_string_intern(X, fname, strlen(fname), 0);
        bool found = false;
        XrValue field_val = key ? xr_map_get(src, xr_string_value(key), &found) : xr_null();
        if (!found) {
            if (xr_json_value_kind_is_nullable(field->json_decode_schema.value_kind)) {
                decoded_values[fi] = xr_null();
                continue;
            }
            xr_json_decode_release_partial(decoded_values, fi);
            xr_free(decoded_values);
            return xr_null();
        }
        XrValue decoded = xr_null();
        if (!xr_json_decode_value_with_schema(X, coro, field_val, &field->json_decode_schema,
                                              ignore_unknown_fields, &decoded)) {
            xr_json_decode_release_partial(decoded_values, fi);
            xr_free(decoded_values);
            return xr_null();
        }
        decoded_values[fi] = decoded;
    }

    XrInstance *result = (cls->flags & XR_CLASS_DYNAMIC_LAYOUT)
                             ? (XrInstance *) xr_object_instance_new_with_class(coro, cls)
                             : xr_instance_new(X, cls);
    if (!result) {
        xr_json_decode_release_partial(decoded_values, field_count);
        xr_free(decoded_values);
        return xr_null();
    }
    for (uint16_t fi = 0; fi < field_count; fi++) {
        bool stored = (cls->flags & XR_CLASS_DYNAMIC_LAYOUT)
                          ? xr_instance_set_dynamic_field(X, (XrObjectInstance *) result, fi,
                                                          decoded_values[fi])
                          : xr_instance_set_decoded_field(result, fi, decoded_values[fi]);
        if (stored)
            continue;
        for (uint16_t remaining = fi; remaining < field_count; remaining++)
            xr_rc_release_value(xr_current_coro_heap(), decoded_values[remaining]);
        xr_rc_release_value(xr_current_coro_heap(), XR_FROM_PTR(result));
        xr_free(decoded_values);
        return xr_null();
    }
    xr_free(decoded_values);
    return XR_FROM_PTR(result);
}

#endif  // XJSON_H
