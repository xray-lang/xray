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
#include "../runtime/mem/xobj_destroy_ops.h"
#include "../runtime/mem/xobj_header.h"
#include "../coro/xaot_await.h"
#include "../coro/xaot_coro.h"
#include "../coro/xaot_task.h"

typedef struct XrString XrString;
XR_FUNC XrString *xr_string_intern_core(struct XrRuntimeCore *core, const char *chars,
                                        size_t length, uint32_t hash);

typedef struct XrAotRuntimeStringView {
    XrObjHeader hdr;
    uint32_t length;
    uint32_t hash;
    char data[];
} XrAotRuntimeStringView;

/* Byte-compatible mirror of VM XrArray (see src/runtime/object/xarray.h): the
 * bridge reads VM-layout arrays through this view, so it must embed the exact
 * same shared field macro plus the VM-only data_on_region_heap tail. */
typedef struct XrAotRuntimeArrayView {
    XrObjHeader hdr;
    XR_ARRAY_ABI_FIELDS;
    uint8_t data_on_region_heap;
    uint8_t pad[2];
} XrAotRuntimeArrayView;

typedef struct XrAotRuntimeMapView {
    XrObjHeader hdr;
    void *owner_heap;
    XR_MAP_ABI_FIELDS;
} XrAotRuntimeMapView;

typedef struct XrAotRuntimeSetView {
    XrObjHeader hdr;
    void *owner_heap;
    XR_SET_ABI_FIELDS;
} XrAotRuntimeSetView;

/* Byte-compatible mirror of VM XrAtomic (see src/runtime/object/xatomic.h).
 * Generated AOT code uses this narrow view for statically typed Atomic<int>
 * fast paths without pulling the VM's full xvalue.h into the AOT prelude. */
typedef struct XrAotRuntimeAtomicView {
    XrObjHeader hdr;
    _Atomic(int64_t) value;
    uint8_t kind;
} XrAotRuntimeAtomicView;

static inline XrAotRuntimeAtomicView *xr_aot_atomic_view(XrValue value) {
    return (XrAotRuntimeAtomicView *) value.ptr;
}

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

    XrValue out = xrt_array_with_capacity((int64_t) payload_count + 1);
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

static inline XrValue xr_aot_bridge_xrt_string_to_runtime(const XrAotContext *ctx, XrValue value) {
    if (!XR_IS_STR(value))
        return value;
    if (!ctx || !ctx->runtime)
        return XR_NULL_VAL;

    const xrt_str_t *src = xr_str_hdr(value);
    if (!src || !src->data || src->len < 0)
        return XR_NULL_VAL;

    struct XrRuntimeCore *core = xr_aot_runtime_core(ctx->runtime);
    if (!core)
        return XR_NULL_VAL;

    XrString *dst = xr_string_intern_core(core, src->data, (size_t) src->len, src->hash);
    return dst ? xr_mkheap(dst, XR_TSTRING) : XR_NULL_VAL;
}

static inline XrValue xr_aot_bridge_xrt_to_runtime(const XrAotContext *ctx, XrValue value) {
    if (XR_IS_STR(value))
        return xr_aot_bridge_xrt_string_to_runtime(ctx, value);
    return value;
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
    dst->adt_enum_name = src->adt_enum_name;
    dst->adt_member_name = src->adt_member_name;
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

static inline XrValue xr_aot_bridge_map_to_xrt(XrValue value) {
    XrAotRuntimeMapView *src = (XrAotRuntimeMapView *) value.ptr;
    if (!src)
        return XR_NULL_VAL;

    XrValue dst_value = xrt_map_new((int64_t) src->count);
    xrt_map_t *dst = (xrt_map_t *) dst_value.ptr;
    dst->key_tid = src->key_tid;
    dst->value_tid = src->value_tid;
    if ((src->flags & (XR_MAP_FLAG_DUMMY | XR_MAP_FLAG_WEAK)) || src->count == 0 || !src->entries)
        return dst_value;

    for (uint32_t i = 0; i < src->nentries; i++) {
        XrMapEntry *entry = &src->entries[i];
        if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        xrt_map_set(dst, xr_aot_bridge_value_to_xrt(entry->key),
                    xr_aot_bridge_value_to_xrt(entry->value));
    }
    return dst_value;
}

static inline XrValue xr_aot_bridge_set_to_xrt(XrValue value) {
    XrAotRuntimeSetView *src = (XrAotRuntimeSetView *) value.ptr;
    if (!src)
        return XR_NULL_VAL;

    XrValue dst_value = xrt_set_new((int64_t) src->count);
    xrt_set_t *dst = (xrt_set_t *) dst_value.ptr;
    dst->elem_tid = src->elem_tid;
    if ((src->flags & (XR_SET_FLAG_DUMMY | XR_SET_FLAG_WEAK)) || src->count == 0 || !src->entries)
        return dst_value;

    for (uint32_t i = 0; i < src->nentries; i++) {
        XrSetEntry *entry = &src->entries[i];
        if (entry->val_tt == XR_SET_ENTRY_NIL)
            continue;
        xrt_set_add(dst, xr_aot_bridge_value_to_xrt(entry->value));
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
    if (value.ptr && !(((const XrObjHeader *) value.ptr)->extra & XR_OBJ_AOT_NATIVE)) {
        if (value.heap_type == XR_TMAP)
            return xr_aot_bridge_map_to_xrt(value);
        if (value.heap_type == XR_TSET)
            return xr_aot_bridge_set_to_xrt(value);
    }
    return value;
}

#endif  // XAOT_CORO_BRIDGE_H
