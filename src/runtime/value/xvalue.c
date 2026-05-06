/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue.c - Tagged Union value system implementation
 *
 * KEY CONCEPT:
 *   Implementation of value creation, type checking, and deep equality.
 *   Provides type-safe conversion functions for all heap object types.
 */

#include "xvalue.h"
#include "../../base/xchecks.h"
#include "xtype.h"
#include "../object/xstring.h"
#include "../xisolate_api.h"
#include "../object/xarray.h"
#include "../object/xmap.h"
#include "../object/xjson.h"
#include "../../base/xmalloc.h"
#include "../gc/xgc.h"
#include "../class/xclass.h"  // XR_IS_CLASS_LITE
#include "xtype_names.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// Forward declarations: deep comparison functions
static bool xr_json_equals_deep(XrValue a, XrValue b);
static bool xr_array_equals_deep(XrValue a, XrValue b);
static bool xr_map_equals_deep(XrValue a, XrValue b);

/* ========== Value Creation Functions ========== */

XrValue xr_null(void) {
    return XR_NULL_VAL;
}

XrValue xr_bool(int b) {
    return b ? XR_TRUE_VAL : XR_FALSE_VAL;
}

// xr_int and xr_float are now static inline in xvalue.h

bool xr_value_deep_eq(XrValue a, XrValue b) {
    // Fast path: same tag and same bits
    if (a.tag == b.tag && a.i == b.i) {
        // NaN != NaN special case
        if (a.tag == XR_TAG_F64 && isnan(a.f))
            return false;
        if (a.tag == XR_TAG_PTR && a.ptr != NULL) {
            // For heap objects, may need deep comparison
            goto deep_compare;
        }
        return true;
    }

    // Different tags
    if (a.tag != b.tag) {
        // Mixed int/float comparison
        if (XR_IS_INT(a) && XR_IS_FLOAT(b)) {
            return (double) a.i == b.f;
        }
        if (XR_IS_FLOAT(a) && XR_IS_INT(b)) {
            return a.f == (double) b.i;
        }
        return false;
    }

    // Same tag, different value
    switch (a.tag) {
        case XR_TAG_NULL:
            return true;
        case XR_TAG_BOOL:
            return a.i == b.i;
        case XR_TAG_I64:
            return a.i == b.i;
        case XR_TAG_F64: {
            if (isnan(a.f) && isnan(b.f))
                return false;
            return a.f == b.f;
        }
        case XR_TAG_PTR:
            goto deep_compare;
        default:
            return a.i == b.i;
    }

deep_compare: {
    XrGCHeader *gc_a = (XrGCHeader *) a.ptr;
    XrGCHeader *gc_b = (XrGCHeader *) b.ptr;
    if (gc_a == gc_b)
        return true;
    if (!gc_a || !gc_b)
        return false;
    if (XR_GC_GET_TYPE(gc_a) != XR_GC_GET_TYPE(gc_b))
        return false;

    if (XR_GC_GET_TYPE(gc_a) == XR_TSTRING) {
        XrString *str_a = (XrString *) gc_a;
        XrString *str_b = (XrString *) gc_b;
        if (str_a->length != str_b->length)
            return false;
        return memcmp(str_a->data, str_b->data, str_a->length) == 0;
    }
    if (XR_GC_GET_TYPE(gc_a) == XR_TJSON) {
        return xr_json_equals_deep(a, b);
    }
    if (XR_GC_GET_TYPE(gc_a) == XR_TARRAY) {
        return xr_array_equals_deep(a, b);
    }
    if (XR_GC_GET_TYPE(gc_a) == XR_TMAP) {
        return xr_map_equals_deep(a, b);
    }
    return gc_a == gc_b;
}
}

/* ========== Unified Type ID ========== */

// XrValueTag → XrTypeId lookup table
static const XrTypeId tag_to_typeid[8] = {
    [XR_TAG_NULL] = XR_TID_NULL,     [XR_TAG_BOOL] = XR_TID_BOOL, [XR_TAG_I64] = XR_TID_INT,
    [XR_TAG_F64] = XR_TID_FLOAT,     [XR_TAG_PTR] = XR_TID_NULL,  [XR_TAG_STRUCT_REF] = XR_TID_NULL,
    [XR_TAG_NOTFOUND] = XR_TID_NULL,
};

// XrObjType (GC header) → XrTypeId lookup table
static const XrTypeId gctype_to_typeid[XR_TTASK + 1] = {
    [XR_TNULL] = XR_TID_NULL,
    [XR_TBOOL] = XR_TID_BOOL,
    [XR_TINT] = XR_TID_INT,
    [XR_TFLOAT] = XR_TID_FLOAT,
    [XR_TSTRING] = XR_TID_STRING,
    [XR_TFUNCTION] = XR_TID_FUNCTION,
    [XR_TCFUNCTION] = XR_TID_FUNCTION,
    [XR_TARRAY] = XR_TID_ARRAY,
    [XR_TSET] = XR_TID_SET,
    [XR_TMAP] = XR_TID_MAP,
    [XR_TCLASS] = XR_TID_FUNCTION,
    [XR_TCLASS_BUILDER_UNUSED] = XR_TID_NULL,
    [XR_TINSTANCE] = XR_TID_INSTANCE,
    [XR_TBOUND_METHOD] = XR_TID_BOUND_METHOD,
    [XR_TENUM_TYPE] = XR_TID_ENUM_TYPE,
    [XR_TENUM_VALUE] = XR_TID_ENUM_VALUE,
    [XR_TERROR] = XR_TID_EXCEPTION,
    [XR_TEXCEPTION] = XR_TID_EXCEPTION,
    [XR_TMODULE] = XR_TID_MODULE,
    [XR_TITERATOR] = XR_TID_ITERATOR,
    [XR_TRESERVED] = XR_TID_NULL,
    [XR_TSTRINGBUILDER] = XR_TID_STRINGBUILDER,
    [XR_TJSON] = XR_TID_JSON,
    [XR_TSHAPE] = XR_TID_NULL,
    [XR_TCOROUTINE] = XR_TID_COROUTINE,
    [XR_TCHANNEL] = XR_TID_CHANNEL,
    [XR_TBIGINT] = XR_TID_BIGINT,
    [XR_TCOROPOOL] = XR_TID_NULL,
    [XR_TARRAY_SLICE] = XR_TID_ARRAY,
    [XR_TDATETIME] = XR_TID_DATETIME,
    [XR_TREGEX] = XR_TID_REGEX,
    [XR_TLOGGER] = XR_TID_NULL,
    [XR_TRANGE] = XR_TID_RANGE,
    [XR_TTASK] = XR_TID_TASK,
};

XrTypeId xr_value_typeid(XrValue v) {
    XR_DCHECK(v.tag <= XR_TAG_NOTFOUND, "value_typeid: invalid tag");
    if (v.tag <= XR_TAG_F64)
        return tag_to_typeid[v.tag];
    if (v.tag == XR_TAG_PTR && v.ptr) {
        uint8_t gctype = XR_GC_GET_TYPE((XrGCHeader *) v.ptr);
        if (gctype < sizeof(gctype_to_typeid) / sizeof(gctype_to_typeid[0]))
            return gctype_to_typeid[gctype];
    }
    return XR_TID_NULL;
}

// XrTypeId → name string lookup table (shared with xtype_names.c)
XR_DATA const char *typeid_names[XR_TID_COUNT] = {
    [XR_TID_NULL] = TYPE_NAME_NULL,
    [XR_TID_BOOL] = TYPE_NAME_BOOL,
    [XR_TID_INT8] = TYPE_NAME_INT8,
    [XR_TID_UINT8] = TYPE_NAME_UINT8,
    [XR_TID_INT16] = TYPE_NAME_INT16,
    [XR_TID_UINT16] = TYPE_NAME_UINT16,
    [XR_TID_INT32] = TYPE_NAME_INT32,
    [XR_TID_UINT32] = TYPE_NAME_UINT32,
    [XR_TID_INT] = TYPE_NAME_INT,
    [XR_TID_UINT64] = TYPE_NAME_UINT64,
    [XR_TID_FLOAT32] = TYPE_NAME_FLOAT32,
    [XR_TID_FLOAT] = TYPE_NAME_FLOAT,
    [XR_TID_STRING] = TYPE_NAME_STRING,
    [XR_TID_FUNCTION] = TYPE_NAME_FUNCTION,
    [XR_TID_ARRAY] = TYPE_NAME_ARRAY,
    [XR_TID_SET] = TYPE_NAME_SET,
    [XR_TID_MAP] = TYPE_NAME_MAP,
    [XR_TID_INSTANCE] = TYPE_NAME_INSTANCE,
    [XR_TID_JSON] = TYPE_NAME_JSON,
    [XR_TID_BIGINT] = TYPE_NAME_BIGINT,
    [XR_TID_STRINGBUILDER] = TYPE_NAME_STRINGBUILDER,
    [XR_TID_CHANNEL] = TYPE_NAME_CHANNEL,
    [XR_TID_REGEX] = TYPE_NAME_REGEX,
    [XR_TID_DATETIME] = TYPE_NAME_DATETIME,
    [XR_TID_EXCEPTION] = TYPE_NAME_EXCEPTION,
    [XR_TID_ENUM_VALUE] = TYPE_NAME_ENUM_VALUE,
    [XR_TID_ENUM_TYPE] = TYPE_NAME_ENUM_TYPE,
    [XR_TID_BOUND_METHOD] = TYPE_NAME_FUNCTION,
    [XR_TID_ITERATOR] = TYPE_NAME_ITERATOR,
    [XR_TID_MODULE] = TYPE_NAME_MODULE,
    [XR_TID_COROUTINE] = TYPE_NAME_COROUTINE,
    [XR_TID_RANGE] = TYPE_NAME_RANGE,
    [XR_TID_TASK] = TYPE_NAME_TASK,
};

const char *xr_typeid_name(XrTypeId tid) {
    if (tid >= 0 && tid < XR_TID_COUNT && typeid_names[tid])
        return typeid_names[tid];
    return TYPE_NAME_UNKNOWN;
}

/* ========== XrType → XrTypeId Conversion (Reified Generics) ========== */

uint8_t xr_type_to_tid(const XrType *type) {
    if (!type)
        return 0;
    XR_DCHECK(type->kind < XR_KIND_COUNT, "type_to_tid: invalid kind");
    switch (type->kind) {
        case XR_KIND_UNKNOWN:
            return 0;
        case XR_KIND_INT:
            return XR_TID_INT;
        case XR_KIND_FLOAT:
            return XR_TID_FLOAT;
        case XR_KIND_STRING:
            return XR_TID_STRING;
        case XR_KIND_BOOL:
            return XR_TID_BOOL;
        case XR_KIND_ARRAY:
            return XR_TID_ARRAY;
        case XR_KIND_MAP:
            return XR_TID_MAP;
        case XR_KIND_SET:
            return XR_TID_SET;
        case XR_KIND_CHANNEL:
            return XR_TID_CHANNEL;
        case XR_KIND_JSON:
            return XR_TID_JSON;
        case XR_KIND_INSTANCE:
            return XR_TID_INSTANCE;
        case XR_KIND_FUNCTION:
            return XR_TID_FUNCTION;
        case XR_KIND_ENUM:
            return XR_TID_ENUM_VALUE;
        default:
            return 0;
    }
}

/* ========== String Data Access (always heap, no SSO) ========== */

const char *xr_value_str_data(const XrValue *v) {
    XR_DCHECK(v != NULL, "value_str_data: NULL value");
    XR_DCHECK(v->ptr != NULL, "value_str_data: NULL ptr");
    return ((XrString *) v->ptr)->data;
}

uint32_t xr_value_str_len(const XrValue *v) {
    XR_DCHECK(v != NULL, "value_str_len: NULL value");
    XR_DCHECK(v->ptr != NULL, "value_str_len: NULL ptr");
    return ((XrString *) v->ptr)->length;
}

/* ========== Object Operations ========== */
XrValue xr_string_value(XrString *str) {
    XR_DCHECK(str != NULL, "string_value: NULL string");
    return XR_FROM_STR(str);
}

/* ========== Value Operations Generators ========== */

// Pattern 1: Types with XR_IS_* macro (null-safe to_* conversion)
#define DEFINE_VALUE_OPS_WITH_MACRO(name, check_macro, cast_type)                                  \
    XrValue xr_value_from_##name(cast_type *obj) {                                                 \
        return XR_FROM_PTR(obj);                                                                   \
    }                                                                                              \
    bool xr_value_is_##name(XrValue v) {                                                           \
        return check_macro(v);                                                                     \
    }                                                                                              \
    cast_type *xr_value_to_##name(XrValue v) {                                                     \
        return check_macro(v) ? (cast_type *) XR_TO_PTR(v) : NULL;                                 \
    }

// Pattern 2: Types with XR_T* type enum (null-safe to_* conversion)
// Uses cached heap_type in XrValue descriptor instead of dereferencing GC header
#define DEFINE_VALUE_OPS_WITH_TYPE(name, type_enum, cast_type)                                     \
    XrValue xr_value_from_##name(cast_type *obj) {                                                 \
        return XR_FROM_PTR(obj);                                                                   \
    }                                                                                              \
    bool xr_value_is_##name(XrValue v) {                                                           \
        return XR_IS_PTR(v) && (v).heap_type == (type_enum);                                       \
    }                                                                                              \
    cast_type *xr_value_to_##name(XrValue v) {                                                     \
        return xr_value_is_##name(v) ? (cast_type *) XR_TO_PTR(v) : NULL;                          \
    }

/* ========== Container Value Operations ========== */
DEFINE_VALUE_OPS_WITH_MACRO(array, XR_IS_ARRAY, XrArray)
DEFINE_VALUE_OPS_WITH_MACRO(map, XR_IS_MAP, struct XrMap)
DEFINE_VALUE_OPS_WITH_MACRO(set, XR_IS_SET, struct XrSet)
DEFINE_VALUE_OPS_WITH_MACRO(module, XR_IS_MODULE, struct XrModule)

/* ========== OOP Value Operations ========== */
DEFINE_VALUE_OPS_WITH_TYPE(class, XR_TCLASS, struct XrClass)
DEFINE_VALUE_OPS_WITH_TYPE(instance, XR_TINSTANCE, struct XrInstance)

/* ========== Function Value Operations ========== */
DEFINE_VALUE_OPS_WITH_MACRO(closure, XR_IS_FUNCTION, struct XrClosure)
DEFINE_VALUE_OPS_WITH_TYPE(cfunction, XR_TCFUNCTION, struct XrCFunction)

/* ========== Coroutine Value Operations ========== */
DEFINE_VALUE_OPS_WITH_TYPE(coro, XR_TCOROUTINE, struct XrCoroutine)

/* ========== Task Value Operations ========== */
DEFINE_VALUE_OPS_WITH_TYPE(task, XR_TTASK, struct XrTask)

/* ========== DateTime Value Operations ========== */
bool xr_value_is_datetime(XrValue v) {
    return XR_IS_PTR(v) && (v).heap_type == XR_TDATETIME;
}

void *xr_value_to_datetime(XrValue v) {
    return xr_value_is_datetime(v) ? XR_TO_PTR(v) : NULL;
}

/* ========== Deep Value Comparison ========== */
static bool xr_json_equals_deep(XrValue a, XrValue b) {
    XR_DCHECK(XR_IS_PTR(a), "json_equals_deep: a is not PTR");
    XR_DCHECK(XR_IS_PTR(b), "json_equals_deep: b is not PTR");
    XrJson *ja = xr_value_to_json(a);
    XrJson *jb = xr_value_to_json(b);
    if (!ja || !jb)
        return false;

    // Compare field count and content
    XrayIsolate *X = xray_isolate_current();
    XrShape *sa = xr_json_shape(X, ja);
    XrShape *sb = xr_json_shape(X, jb);
    if (sa->field_count != sb->field_count)
        return false;

    for (int i = 0; i < sa->field_count; i++) {
        SymbolId sym_a = sa->field_symbols[i];
        int idx_b = -1;
        for (int j = 0; j < sb->field_count; j++) {
            if (sb->field_symbols[j] == sym_a) {
                idx_b = j;
                break;
            }
        }
        if (idx_b < 0)
            return false;

        if (!xr_value_deep_eq(ja->fields[i], jb->fields[idx_b])) {
            return false;
        }
    }
    return true;
}

static bool xr_array_equals_deep(XrValue a, XrValue b) {
    XR_DCHECK(XR_IS_PTR(a), "array_equals_deep: a is not PTR");
    XR_DCHECK(XR_IS_PTR(b), "array_equals_deep: b is not PTR");
    XrArray *aa = xr_value_to_array(a);
    XrArray *ab = xr_value_to_array(b);
    if (!aa || !ab)
        return false;

    if (aa->length != ab->length)
        return false;

    for (int i = 0; i < aa->length; i++) {
        if (!xr_value_deep_eq(xr_array_get_element(aa, i), xr_array_get_element(ab, i))) {
            return false;
        }
    }
    return true;
}

static bool xr_map_equals_deep(XrValue a, XrValue b) {
    XR_DCHECK(XR_IS_PTR(a), "map_equals_deep: a is not PTR");
    XR_DCHECK(XR_IS_PTR(b), "map_equals_deep: b is not PTR");
    XrMap *ma = xr_value_to_map(a);
    XrMap *mb = xr_value_to_map(b);
    if (!ma || !mb)
        return false;

    if (ma->count != mb->count)
        return false;

    // Iterate all nodes in ma, check if same key-value exists in mb
    uint32_t size = xr_map_sizenode(ma);
    for (uint32_t i = 0; i < size; i++) {
        XrMapNode *node = xr_map_node(ma, i);
        if (XR_MAP_NODE_EMPTY(node))
            continue;

        // Find same key in mb
        bool found = false;
        XrValue val_b = xr_map_get(mb, node->key, &found);
        if (!found)
            return false;

        // Compare values recursively
        if (!xr_value_deep_eq(node->value, val_b)) {
            return false;
        }
    }
    return true;
}
