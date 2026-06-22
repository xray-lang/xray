/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_deep_eq.c - Deep value equality.
 */

#include "xvalue.h"
#include "../class/xclass.h"
#include "../class/xinstance.h"
#include "../gc/xgc_header.h"
#include "../object/xarray.h"
#include "../object/xjson.h"
#include "../object/xmap.h"
#include "../object/xstring.h"
#include <math.h>
#include <string.h>

static bool xr_json_equals_deep(XrValue a, XrValue b);
static bool xr_array_equals_deep(XrValue a, XrValue b);
static bool xr_map_equals_deep(XrValue a, XrValue b);

bool xr_value_deep_eq(XrValue a, XrValue b) {
    if (a.tag == b.tag && a.i == b.i) {
        if (a.tag == XR_TAG_F64 && isnan(a.f))
            return false;
        if (a.tag == XR_TAG_PTR && a.ptr != NULL)
            goto deep_compare;
        return true;
    }

    if (a.tag != b.tag) {
        if (XR_IS_INT(a) && XR_IS_FLOAT(b))
            return (double) a.i == b.f;
        if (XR_IS_FLOAT(a) && XR_IS_INT(b))
            return a.f == (double) b.i;
        return false;
    }

    switch (a.tag) {
        case XR_TAG_NULL:
            return true;
        case XR_TAG_BOOL:
        case XR_TAG_I64:
            return a.i == b.i;
        case XR_TAG_F64:
            if (isnan(a.f) && isnan(b.f))
                return false;
            return a.f == b.f;
        case XR_TAG_PTR:
            goto deep_compare;
        default:
            return a.i == b.i;
    }

deep_compare: {
    XrObjHeader *gc_a = (XrObjHeader *) a.ptr;
    XrObjHeader *gc_b = (XrObjHeader *) b.ptr;
    if (gc_a == gc_b)
        return true;
    if (!gc_a || !gc_b)
        return false;
    if (XR_OBJ_GET_TYPE(gc_a) != XR_OBJ_GET_TYPE(gc_b))
        return false;

    if (XR_OBJ_GET_TYPE(gc_a) == XR_TSTRING) {
        XrString *str_a = (XrString *) gc_a;
        XrString *str_b = (XrString *) gc_b;
        if (str_a->length != str_b->length)
            return false;
        return memcmp(str_a->data, str_b->data, str_a->length) == 0;
    }
    if (XR_OBJ_GET_TYPE(gc_a) == XR_TINSTANCE) {
        XrInstance *ia = (XrInstance *) gc_a;
        if (ia->klass && ia->klass->builtin_kind == XR_BK_JSON)
            return xr_json_equals_deep(a, b);
    }
    if (XR_OBJ_GET_TYPE(gc_a) == XR_TARRAY)
        return xr_array_equals_deep(a, b);
    if (XR_OBJ_GET_TYPE(gc_a) == XR_TMAP)
        return xr_map_equals_deep(a, b);
    return gc_a == gc_b;
}
}

static bool xr_json_equals_deep(XrValue a, XrValue b) {
    XrJson *ja = xr_value_to_json(a);
    XrJson *jb = xr_value_to_json(b);
    if (!ja || !jb)
        return false;

    XrClass *ca = ja->klass;
    XrClass *cb = jb->klass;
    if (!ca || !cb || ca->field_count != cb->field_count)
        return false;

    for (int i = 0; i < ca->field_count; i++) {
        int sym_a = ca->fields[i].symbol;
        int idx_b = xr_class_lookup_field(cb, sym_a);
        if (idx_b < 0)
            return false;
        XrValue va = xr_instance_get_dynamic_field(ja, (uint16_t) i);
        XrValue vb = xr_instance_get_dynamic_field(jb, (uint16_t) idx_b);
        if (!xr_value_deep_eq(va, vb))
            return false;
    }
    return true;
}

static bool xr_array_equals_deep(XrValue a, XrValue b) {
    XrArray *aa = xr_value_to_array(a);
    XrArray *ab = xr_value_to_array(b);
    if (!aa || !ab)
        return false;
    if (aa->length != ab->length)
        return false;

    for (int i = 0; i < aa->length; i++) {
        if (!xr_value_deep_eq(xr_array_get_element(aa, i), xr_array_get_element(ab, i)))
            return false;
    }
    return true;
}

static bool xr_map_equals_deep(XrValue a, XrValue b) {
    XrMap *ma = xr_value_to_map(a);
    XrMap *mb = xr_value_to_map(b);
    if (!ma || !mb)
        return false;
    if (ma->count != mb->count)
        return false;

    for (uint32_t i = 0; i < ma->nentries; i++) {
        XrMapEntry *node = xr_map_entry(ma, i);
        if (XR_MAP_ENTRY_EMPTY(node))
            continue;

        bool found = false;
        XrValue val_b = xr_map_get(mb, node->key, &found);
        if (!found)
            return false;
        if (!xr_value_deep_eq(node->value, val_b))
            return false;
    }
    return true;
}
