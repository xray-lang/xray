/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_array_local.h - Guarded scalar edits of verified private Frame bindings
 */
#ifndef XRAY_NATIVE_ARRAY_LOCAL_H
#define XRAY_NATIVE_ARRAY_LOCAL_H
#include "xray_native_array.h"
#include "xray_native_managed_root.h"
#ifndef __cplusplus
#include <string.h>

typedef struct XrtArrayI64LocalGeometry {
    size_t binding_slot, binding_offset, workspace_slot, workspace_offset;
} XrtArrayI64LocalGeometry;
/* Only addresses in a live, fixed Frame are borrowed. In particular neither
 * the factory spec nor any owner/backing/uniqueness result survives prepare.
 * The view expires at Frame leave or Root drain; it has no cleanup action. */
typedef struct XrtArrayI64LocalView {
    const bool *busy;
    const XrtManagedPending *binding_pending, *workspace_pending;
    XrtManagedHeader *const *binding_owner, *const *workspace_owner;
} XrtArrayI64LocalView;
typedef struct XrtArrayI64LocalUpdate {
    XrtArrayUpdateKind operation;
    int64_t index, value;
} XrtArrayI64LocalUpdate;

typedef struct XrtArrayI64RegionSlot { size_t slot, offset; } XrtArrayI64RegionSlot;
typedef struct XrtArrayI64RegionSpec {
    const XrtArrayI64RegionSlot *slots;
    size_t slot_count, canonical;
} XrtArrayI64RegionSpec;
typedef struct XrtArrayI64RegionContext {
    XrtManagedFrame *frame;
    void *payload;
    const XrtManagedFrameLocations *locations;
    const XrtManagedFrameSpec *spec;
} XrtArrayI64RegionContext;
/* These fields borrow live bytes. They own no reference or dynamic right and
 * must never be read after Frame/Root retirement. Clearing the local struct
 * needs no Frame access, unlock, drop or other retirement operation. */
typedef struct XrtArrayI64RegionCache {
    XrtManagedHeader *owner;
    unsigned char *elements;
    size_t length;
} XrtArrayI64RegionCache;

#if defined(_MSC_VER) && !defined(__clang__)
#define XRT_ARRAY_LOCAL_INLINE static __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define XRT_ARRAY_LOCAL_INLINE static inline __attribute__((always_inline))
#else
#define XRT_ARRAY_LOCAL_INLINE static inline
#endif

/* This is the only full dynamic check for a producer-described call-free
 * region. Ready means its original complete SET/READ/drop/latch has returned,
 * not a metadata permission. Inputs are actual live private factory objects
 * and a numeric projection of exact I64 homes. Typed setup supplies the real
 * allocation extent; this is not validation of arbitrary or expired pointers.
 * Success borrows bytes without retaining or changing busy/pending state.
 * The caller preserves real owner moves and either original per-access bounds
 * checks or its exact conditional arithmetic proof in the uninterrupted scalar
 * window. No numeric fact grants access. False changes no input or output byte. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_region_try_begin(const XrtArrayI64RegionContext *context,
    const XrtArrayI64RegionSpec *region, bool ready, XrtArrayI64RegionCache *empty) {
    if (!context || !region || !empty || empty->owner || empty->elements || empty->length || !ready ||
        !context->frame || !context->payload || !context->locations || !context->spec ||
        !region->slots || !region->slot_count || region->canonical >= region->slot_count) return false;
    const XrtManagedFrameLocations *locations = context->locations;
    const XrtManagedFrameSpec *spec = context->spec;
    void *payload = context->payload;
    if (locations->frame != context->frame || locations->payload != payload || !locations->slots || !locations->busy ||
        !spec->owned_offsets || spec->slot_count != locations->slot_count || region->slot_count > spec->slot_count ||
        spec->payload_alignment < _Alignof(XrtManagedHeader *) ||
        (spec->payload_alignment & (spec->payload_alignment - 1)) || (uintptr_t)payload % spec->payload_alignment)
        return false;
    if (*locations->busy) return false;
    XrtManagedHeader *owner = NULL;
    for (size_t n = 0; n < region->slot_count; ++n) {
        size_t slot = region->slots[n].slot, offset = region->slots[n].offset;
        if (slot >= locations->slot_count || offset > spec->payload_size ||
            sizeof(XrtManagedHeader *) > spec->payload_size - offset || offset % _Alignof(XrtManagedHeader *) ||
            spec->owned_offsets[slot] != offset || locations->slots[slot].owner_offset != offset ||
            locations->slots[slot].pending.value || locations->slots[slot].pending.allocation_bytes) return false;
        XrtManagedHeader *value = *(XrtManagedHeader **)((unsigned char *)payload + offset);
        if (n == region->canonical) { if (!value) return false; owner = value; }
        else if (value) return false;
    }
    if (!owner || atomic_load_explicit(&owner->references, memory_order_relaxed) != 1) return false;
    XrtArrayBacking *array = (XrtArrayBacking *)owner;
    size_t maximum = (SIZE_MAX - sizeof(*array)) / sizeof(int64_t);
    if (array->length > array->capacity || array->capacity > maximum || (uint64_t)array->length > (uint64_t)INT64_MAX)
        return false;
    *empty = (XrtArrayI64RegionCache){owner,array->elements,array->length};
    return true;
}

/* Actual private factory objects provide the immutable geometry. Preparation
 * allocates nothing and does not inspect live owner/pending/busy state. It may
 * run before backing initialization. False leaves an empty output unchanged. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_local_prepare(XrtManagedFrame *frame, void *payload,
    const XrtManagedFrameLocations *locations, const XrtManagedFrameSpec *spec,
    XrtArrayI64LocalGeometry geometry, XrtArrayI64LocalView *empty) {
    if (!empty || empty->busy || empty->binding_pending || empty->workspace_pending ||
        empty->binding_owner || empty->workspace_owner ||
        !frame || !payload || !locations || !spec || locations->frame!=frame || locations->payload!=payload ||
        !locations->slots || !locations->busy || !spec->owned_offsets ||
        spec->slot_count!=locations->slot_count || !spec->payload_alignment ||
        (spec->payload_alignment & (spec->payload_alignment-1)) ||
        spec->payload_alignment<_Alignof(XrtManagedHeader *) || (uintptr_t)payload%spec->payload_alignment ||
        geometry.binding_slot>=locations->slot_count || geometry.workspace_slot>=locations->slot_count ||
        geometry.binding_slot==geometry.workspace_slot || geometry.binding_offset==geometry.workspace_offset ||
        geometry.binding_offset==SIZE_MAX || geometry.workspace_offset==SIZE_MAX) return false;
    size_t slots[]={geometry.binding_slot,geometry.workspace_slot};
    size_t offsets[]={geometry.binding_offset,geometry.workspace_offset};
    for (size_t n=0;n<2;++n)
        if (offsets[n]>spec->payload_size || sizeof(XrtManagedHeader *)>spec->payload_size-offsets[n] ||
            offsets[n]%_Alignof(XrtManagedHeader *) || spec->owned_offsets[slots[n]]!=offsets[n] ||
            locations->slots[slots[n]].owner_offset!=offsets[n]) return false;
    *empty=(XrtArrayI64LocalView){locations->busy,&locations->slots[slots[0]].pending,
        &locations->slots[slots[1]].pending,
        (XrtManagedHeader **)((unsigned char *)payload+offsets[0]),
        (XrtManagedHeader **)((unsigned char *)payload+offsets[1])};
    return true;
}

/* A prepared view confers no mutation authority. The caller still supplies an
 * admitted LOCAL I64 operation after real backing initialization. Every try
 * reads current busy, pending, actual owners, RC and backing bounds in order.
 * Only the uninterrupted success window edits bytes; false changes nothing
 * and requires the ordinary kernel with fresh reads. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_try_update_local(const XrtArrayI64LocalView *view,
    XrtArrayI64LocalUpdate update) {
    if (!view || !view->busy || !view->binding_pending || !view->workspace_pending ||
        !view->binding_owner || !view->workspace_owner) return false;
    if (*view->busy) return false;
    if (view->binding_pending->value || view->binding_pending->allocation_bytes ||
        view->workspace_pending->value || view->workspace_pending->allocation_bytes) return false;
    XrtManagedHeader *owner=*view->binding_owner;
    if (!owner || *view->workspace_owner ||
        atomic_load_explicit(&owner->references,memory_order_relaxed)!=1) return false;
    XrtArrayBacking *array=(XrtArrayBacking *)owner;
    size_t maximum=(SIZE_MAX-sizeof(*array))/sizeof(int64_t), index=0;
    if (array->length>array->capacity || array->capacity>maximum) return false;
    if (update.operation==XRT_ARRAY_UPDATE_SET) {
        if (update.index<0 || (uint64_t)update.index>=array->length) return false;
        index=(size_t)update.index;
    } else if (update.operation==XRT_ARRAY_UPDATE_PUSH) {
        if (array->length>=array->capacity || array->length>=maximum || (uint64_t)array->length>=(uint64_t)INT64_MAX)
            return false;
        index=array->length;
    } else return false;
    memcpy(array->elements+index*sizeof(int64_t),&update.value,sizeof(update.value));
    if (update.operation==XRT_ARRAY_UPDATE_PUSH) array->length=index+1;
    return true;
}
#undef XRT_ARRAY_LOCAL_INLINE
#endif
#endif // XRAY_NATIVE_ARRAY_LOCAL_H
