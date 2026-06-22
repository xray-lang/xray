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
#include "../runtime/gc/xgc_destroy_ops.h"
#include "../runtime/gc/xgc_header.h"
#include "../coro/xaot_await.h"
#include "../coro/xaot_coro.h"
#include "../coro/xaot_task.h"

typedef struct XrAotRuntimeStringView {
    XrObjHeader hdr;
    uint32_t length;
    uint32_t hash;
    char data[];
} XrAotRuntimeStringView;

/* Byte-compatible mirror of VM XrArray (see src/runtime/object/xarray.h): the
 * bridge reads VM-layout arrays through this view, so it must embed the exact
 * same shared field macro plus the VM-only data_on_gc_heap tail. */
typedef struct XrAotRuntimeArrayView {
    XrObjHeader hdr;
    XR_ARRAY_ABI_FIELDS;
    uint8_t data_on_gc_heap;
    uint8_t pad[2];
} XrAotRuntimeArrayView;

static inline XrValue xr_aot_bridge_value_to_xrt(XrValue value);

static inline XrValue xr_aot_bridge_enum_key_to_xrt(XrValue value, uint32_t member_index) {
    XrValue out = value;
    out.tag = XR_TAG_ENUM;
    out.ext = member_index;
    return out;
}

static inline XrValue xr_aot_bridge_runtime_enum_to_xrt(XrValue value) {
    uint32_t member_index = 0;
    bool is_adt = false;
    int payload_count = 0;
    if (!xr_aot_runtime_enum_value_info(value, NULL, NULL, &member_index, &is_adt,
                                        &payload_count)) {
        return value;
    }
    if (is_adt && payload_count > 0)
        return XR_FROM_INT((int64_t) member_index);
    return xr_aot_bridge_enum_key_to_xrt(value, member_index);
}

static inline XrValue xr_aot_bridge_runtime_adt_to_xrt(XrValue value) {
    const char *enum_name = NULL;
    const char *member_name = NULL;
    uint32_t member_index = 0;
    int payload_count = 0;
    if (!xr_aot_runtime_adt_value_info(value, &enum_name, &member_name, &member_index,
                                       &payload_count)) {
        return value;
    }

    XrValue out = xrt_array_new((int64_t) payload_count + 1);
    xrt_array_t *arr = (xrt_array_t *) out.ptr;
    arr->adt_enum_name = enum_name;
    arr->adt_member_name = member_name;
    xrt_array_push(out, XR_FROM_INT((int64_t) member_index));
    for (int i = 0; i < payload_count; i++) {
        XrValue payload = xr_aot_runtime_adt_payload(value, i);
        xrt_array_push(out, xr_aot_bridge_value_to_xrt(payload));
    }
    return out;
}

static inline XrValue xr_aot_bridge_string_to_xrt(XrValue value) {
    XrAotRuntimeStringView *src = (XrAotRuntimeStringView *) value.ptr;
    if (!src)
        return XR_NULL_VAL;

    XrValue dst = xrt_str_alloc((size_t) src->length);
    char *data = xr_str_buf(dst);
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
    dst->length = (int64_t) src->length;
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
    XrValue adt = xr_aot_bridge_runtime_adt_to_xrt(value);
    if (adt.tag != value.tag || adt.ptr != value.ptr)
        return adt;
    XrValue enum_value = xr_aot_bridge_runtime_enum_to_xrt(value);
    if (enum_value.tag != value.tag || enum_value.ptr != value.ptr)
        return enum_value;
    if (value.heap_type == XR_TSTRING)
        return xr_aot_bridge_string_to_xrt(value);
    /* Only VM-layout arrays need representation conversion. AOT arrays carry the
     * same byte layout (a shared XrObjHeader at offset 0) and are bump-tagged, so
     * they pass through; the per-coroutine isolation deep-copy is handled
     * separately by xrt_value_clone_for_coro. */
    if (value.heap_type == XR_TARRAY && value.ptr &&
        !(((const XrObjHeader *) value.ptr)->extra & XR_OBJ_STORAGE_BUMP))
        return xr_aot_bridge_array_to_xrt(value);
    return value;
}

#endif  // XAOT_CORO_BRIDGE_H
