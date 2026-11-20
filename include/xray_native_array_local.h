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

/* Keep a read-only window separate from a sibling mutation helper. This is
 * a code layout hint; compilers without it use an ordinary C function. */
#if defined(_MSC_VER) && !defined(__clang__)
#define XRT_ARRAY_REGION_OUTLINE __declspec(noinline)
#elif defined(__clang__) || defined(__GNUC__)
#define XRT_ARRAY_REGION_OUTLINE __attribute__((noinline))
#else
#define XRT_ARRAY_REGION_OUTLINE
#endif

/* The admitted mutation loop copies within checked spans or between exact
 * scalar objects. Expose fixed-width byte access to the optimizer even when
 * libc wraps memcpy in a checked call. Backing remains byte storage. */
#if defined(__clang__) || defined(__GNUC__)
#define XRT_ARRAY_I64_REGION_COPY(destination, source) \
    __builtin_memcpy((destination), (source), sizeof(int64_t))
#else
#define XRT_ARRAY_I64_REGION_COPY(destination, source) \
    memcpy((destination), (source), sizeof(int64_t))
#endif

/* Operands are distinct generated scalar locals. GCC optimizes the modulo
 * loop better when it sees the bit copy. Other providers keep the arithmetic
 * reconstruction to avoid materializing the scratch object in the loop. */
#if defined(__GNUC__) && !defined(__clang__)
#define XRT_ARRAY_I64_REGION_RECONSTRUCT(destination, bits) \
    XRT_ARRAY_I64_REGION_COPY(&(destination), &(bits))
#else
#define XRT_ARRAY_I64_REGION_RECONSTRUCT(destination, bits) \
    ((destination) = (bits) <= (uint64_t)INT64_MAX ? (int64_t)(bits) : \
        -INT64_C(1) - (int64_t)((~(bits)) & UINT64_MAX))
#endif

/* Paired values are a code generation convenience for an admitted byte
 * span. Byte copies preserve the effective type and impose no alignment on
 * the backing. Unsupported providers use the original scalar loop. */
#if defined(__GNUC__) && !defined(__clang__) && \
    ((defined(__x86_64__) && defined(__SSE2__)) || (defined(__aarch64__) && defined(__ARM_NEON)))
#define XRT_ARRAY_I64_PAIR_AVAILABLE 1
typedef uint64_t XrtArrayI64Pair __attribute__((vector_size(16)));

XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_load(const unsigned char *source) {
    XrtArrayI64Pair value;
    __builtin_memcpy(&value, source, sizeof(value));
    return value;
}
XRT_ARRAY_LOCAL_INLINE void xrt_array_i64_pair_store(unsigned char *target, XrtArrayI64Pair value) {
    __builtin_memcpy(target, &value, sizeof(value));
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_splat(uint64_t value) {
    return (XrtArrayI64Pair){value, value};
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_add(XrtArrayI64Pair a, XrtArrayI64Pair b) {
    return a + b;
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_sub(XrtArrayI64Pair a, XrtArrayI64Pair b) {
    return a - b;
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_shl(XrtArrayI64Pair value, int amount) {
    return value << amount;
}

#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) && !defined(_M_ARM64EC)
/* SSE2 is part of the MSVC x64 baseline. Use copies so even a byte-aligned
 * admitted backing does not acquire an intrinsic pointer alignment contract. */
#include <emmintrin.h>
#define XRT_ARRAY_I64_PAIR_AVAILABLE 1
typedef __m128i XrtArrayI64Pair;

XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_load(const unsigned char *source) {
    XrtArrayI64Pair value;
    memcpy(&value, source, sizeof(value));
    return value;
}
XRT_ARRAY_LOCAL_INLINE void xrt_array_i64_pair_store(unsigned char *target, XrtArrayI64Pair value) {
    memcpy(target, &value, sizeof(value));
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_splat(uint64_t value) {
    int64_t lane;
    memcpy(&lane, &value, sizeof(lane));
    return _mm_set1_epi64x(lane);
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_add(XrtArrayI64Pair a, XrtArrayI64Pair b) {
    return _mm_add_epi64(a, b);
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_sub(XrtArrayI64Pair a, XrtArrayI64Pair b) {
    return _mm_sub_epi64(a, b);
}
XRT_ARRAY_LOCAL_INLINE XrtArrayI64Pair xrt_array_i64_pair_shl(XrtArrayI64Pair value, int amount) {
    return _mm_slli_epi64(value, amount);
}
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
 * admitted LOCAL I64 operation after real backing initialization. Acquisition
 * reads current busy, pending, actual owners and RC in order and returns the
 * binding's backing only when this activation holds its sole reference. NULL
 * changes nothing and requires the ordinary kernel with fresh reads. */
XRT_ARRAY_LOCAL_INLINE XrtArrayBacking *xrt_array_i64_local_acquire(const XrtArrayI64LocalView *view) {
    if (!view || !view->busy || !view->binding_pending || !view->workspace_pending ||
        !view->binding_owner || !view->workspace_owner) return NULL;
    if (*view->busy) return NULL;
    if (view->binding_pending->value || view->binding_pending->allocation_bytes ||
        view->workspace_pending->value || view->workspace_pending->allocation_bytes) return NULL;
    XrtManagedHeader *owner=*view->binding_owner;
    if (!owner || *view->workspace_owner ||
        atomic_load_explicit(&owner->references,memory_order_relaxed)!=1) return NULL;
    return (XrtArrayBacking *)owner;
}

/* Edit an acquired sole-owner backing within its current bounds and capacity.
 * False changes nothing; growth and out-of-bounds writes need the kernel. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_local_apply(XrtArrayBacking *array, XrtArrayI64LocalUpdate update) {
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
    /* The element bytes follow two size_t fields of an aligned header, so an
     * I64 lvalue is aligned. A typed store tells the C compiler it cannot
     * change Frame owner pointers, unlike a byte copy that may alias them. */
    ((int64_t *)(void *)array->elements)[index]=update.value;
    if (update.operation==XRT_ARRAY_UPDATE_PUSH) array->length=index+1;
    return true;
}
_Static_assert(offsetof(XrtArrayBacking, elements) % _Alignof(int64_t) == 0,
    "I64 elements start at an aligned offset of the Array backing");

/* Every try reads the actual Frame state; false requires the ordinary kernel. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_try_update_local(const XrtArrayI64LocalView *view,
    XrtArrayI64LocalUpdate update) {
    XrtArrayBacking *array=xrt_array_i64_local_acquire(view);
    return array && xrt_array_i64_local_apply(array,update);
}

/* Generated code keeps one acquisition per binding in *held while it executes
 * only operations that cannot run guest code, retain or release the binding,
 * move its owner or replace its backing; every other operation clears *held
 * first. A held backing therefore still has the binding's sole reference and
 * skips the Frame reads. A miss leaves *held for the caller to clear before
 * the ordinary kernel, which may replace or grow the backing. */
XRT_ARRAY_LOCAL_INLINE bool xrt_array_i64_update_held(XrtArrayBacking **held, const XrtArrayI64LocalView *view,
    XrtArrayI64LocalUpdate update) {
    if (!*held) *held=xrt_array_i64_local_acquire(view);
    return *held && xrt_array_i64_local_apply(*held,update);
}
#undef XRT_ARRAY_LOCAL_INLINE
#endif
#endif // XRAY_NATIVE_ARRAY_LOCAL_H
