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

typedef struct XrtArrayI64LocalUpdate {
    size_t binding_slot, binding_offset, workspace_slot, workspace_offset;
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
 * The caller preserves real owner moves and original per-access bounds checks
 * in the uninterrupted scalar window. False changes no input or output byte. */
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

/* The caller has completed this execution's backing initialization and proves
 * an admitted mutable LOCAL I64 Array operation with a separate workspace.
 * Frame, view and immutable spec are the actual private factory/plan objects,
 * not independently authenticated descriptors. Their active lifetime covers
 * this single-executor window without calls, allocation or observation.
 * RC1 only selects a physical strategy; it grants no mutation authority.
 * False changes nothing and requires the original kernel with fresh reads.
 * Capacity is the real typed allocation's prefix, not arbitrary external data. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_try_update_local(XrtManagedFrame *frame, void *payload,
    const XrtManagedFrameLocations *locations, const XrtManagedFrameSpec *spec,
    XrtArrayI64LocalUpdate update) {
    if (!frame || !payload || !locations || !spec || locations->frame!=frame || locations->payload!=payload ||
        !locations->slots || !locations->busy || !spec->owned_offsets ||
        spec->slot_count!=locations->slot_count || !spec->payload_alignment ||
        (spec->payload_alignment & (spec->payload_alignment-1)) ||
        spec->payload_alignment<_Alignof(XrtManagedHeader *) || (uintptr_t)payload%spec->payload_alignment ||
        update.binding_slot>=locations->slot_count || update.workspace_slot>=locations->slot_count ||
        update.binding_slot==update.workspace_slot || update.binding_offset==update.workspace_offset ||
        update.binding_offset==SIZE_MAX || update.workspace_offset==SIZE_MAX) return false;
    size_t slots[]={update.binding_slot,update.workspace_slot};
    size_t offsets[]={update.binding_offset,update.workspace_offset};
    for (size_t n=0;n<2;++n)
        if (offsets[n]>spec->payload_size || sizeof(XrtManagedHeader *)>spec->payload_size-offsets[n] ||
            offsets[n]%_Alignof(XrtManagedHeader *) || spec->owned_offsets[slots[n]]!=offsets[n] ||
            locations->slots[slots[n]].owner_offset!=offsets[n]) return false;
    if (*locations->busy) return false;
    if (locations->slots[slots[0]].pending.value || locations->slots[slots[0]].pending.allocation_bytes ||
        locations->slots[slots[1]].pending.value || locations->slots[slots[1]].pending.allocation_bytes) return false;
    XrtManagedHeader *owner=*(XrtManagedHeader **)((unsigned char *)payload+offsets[0]);
    if (!owner || *(XrtManagedHeader **)((unsigned char *)payload+offsets[1]) ||
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
