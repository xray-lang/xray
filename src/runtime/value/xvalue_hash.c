/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvalue_hash.c - XrValue-aware hash functions implementation
 */

#include "xvalue_hash.h"
#include "../../base/xchecks.h"
#include "../object/xstring.h"
#include <math.h>

uint32_t xr_hash_string(XrString *str) {
    XR_DCHECK(str != NULL, "hash_string: NULL string");
    if (str->hash == 0) {
        uint32_t h = xr_string_hash(str->data, str->length);
        str->hash = (h == 0) ? 1 : h;
    }
    return str->hash;
}

uint32_t xr_hash_value(XrValue val) {
    switch (val.tag) {
    case XR_TAG_NULL:
        return 6;
    case XR_TAG_BOOL:
        return xr_hash_bool((int)val.i);
    case XR_TAG_I64:
        return xr_hash_int(val.i);
    case XR_TAG_F64:
        return xr_hash_float(val.f);
    case XR_TAG_PTR:
        if (val.heap_type == XR_TSTRING) {
            return xr_hash_string(XR_TO_STRING(val));
        }
        { uintptr_t ptr = (uintptr_t)val.ptr;
          return (uint32_t)(ptr ^ (ptr >> 16)); }
    default:
        return 0;
    }
}

bool xr_value_eq(XrValue a, XrValue b) {
    // Fast path: same tag and value
    if (xr_value_same(a, b)) return true;
    
    XrTypeId tid_a = xr_value_typeid(a);
    XrTypeId tid_b = xr_value_typeid(b);
    
    if (tid_a != tid_b) return false;
    
    if (tid_a == XR_TID_NULL) return true;
    if (tid_a == XR_TID_BOOL) return XR_TO_BOOL(a) == XR_TO_BOOL(b);
    if (XR_TID_IS_INT(tid_a)) return XR_TO_INT(a) == XR_TO_INT(b);
    if (XR_TID_IS_FLOAT(tid_a)) {
        double fa = XR_TO_FLOAT(a);
        double fb = XR_TO_FLOAT(b);
        if (isnan(fa) || isnan(fb)) return false;
        if (fa == 0.0 && fb == 0.0) return true;
        return fa == fb;
    }
    if (tid_a == XR_TID_STRING) {
        XrString *s1 = XR_TO_STRING(a);
        XrString *s2 = XR_TO_STRING(b);
        if (s1 == s2) return true;
        if (s1->length != s2->length) return false;
        return memcmp(s1->data, s2->data, s1->length) == 0;
    }
    // GC objects: pointer (reference) equality
    if (XR_IS_PTR(a) && XR_IS_PTR(b)) {
        return XR_TO_PTR(a) == XR_TO_PTR(b);
    }
    return false;
}
