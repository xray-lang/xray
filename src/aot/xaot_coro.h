/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_coro.h - Generated AOT coroutine bridge include
 */

#ifndef XAOT_CORO_BRIDGE_H
#define XAOT_CORO_BRIDGE_H

#include "xrt_coll.h"
#include "../runtime/gc/xgc_header.h"
#include "../coro/xaot_coro.h"

typedef struct XrAotRuntimeStringView {
    XrGCHeader gc;
    uint32_t length;
    uint32_t hash;
    char data[];
} XrAotRuntimeStringView;

typedef struct XrAotRuntimeArrayView {
    XrGCHeader gc;
    void *data;
    int32_t length;
    int32_t capacity;
    void *source;
    uint8_t elem_type;
    uint8_t elem_size;
    uint8_t elem_tid;
    uint8_t has_gc_ptrs;
    uint8_t data_on_gc_heap;
    uint8_t pad[3];
} XrAotRuntimeArrayView;

static inline XrValue xr_aot_bridge_value_to_xrt(XrValue value);

static inline XrValue xr_aot_bridge_string_to_xrt(XrValue value) {
    XrAotRuntimeStringView *src = (XrAotRuntimeStringView *) value.ptr;
    if (!src)
        return XR_NULL_VAL;

    XrValue dst = xrt_str_alloc((size_t) src->length);
    char *data = (char *) dst.ptr;
    memcpy(data, src->data, (size_t) src->length);
    data[src->length] = '\0';
    return dst;
}

static inline XrValue xr_aot_bridge_array_to_xrt(XrValue value) {
    XrAotRuntimeArrayView *src = (XrAotRuntimeArrayView *) value.ptr;
    if (!src)
        return XR_NULL_VAL;
    if (src->length < 0)
        return XR_NULL_VAL;

    uint8_t elem_type = src->elem_type < XR_ELEM_COUNT ? src->elem_type : XR_ELEM_ANY;
    XrValue dst_value = xrt_array_new_typed((int64_t) src->length, elem_type);
    xrt_array_t *dst = (xrt_array_t *) dst_value.ptr;
    dst->len = (int64_t) src->length;
    if (src->length == 0 || !src->data)
        return dst_value;

    if (elem_type == XR_ELEM_ANY) {
        for (int32_t i = 0; i < src->length; i++) {
            XrValue item = xr_typed_get(src->data, i, elem_type);
            xr_typed_set(dst->data, i, xr_aot_bridge_value_to_xrt(item), elem_type);
        }
    } else {
        memcpy(dst->data, src->data, (size_t) src->length * (size_t) dst->elem_size);
    }
    return dst_value;
}

static inline XrValue xr_aot_bridge_value_to_xrt(XrValue value) {
    if (value.tag != XR_TAG_PTR)
        return value;
    if (value.heap_type == XR_TSTRING)
        return xr_aot_bridge_string_to_xrt(value);
    if (value.heap_type == XR_TARRAY)
        return xr_aot_bridge_array_to_xrt(value);
    return value;
}

#endif  // XAOT_CORO_BRIDGE_H
