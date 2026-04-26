/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeep_copy.h - Deep copy for cross-coroutine value passing
 *
 * KEY CONCEPT:
 *   When passing values between coroutines, mutable objects must be copied.
 *   Each coroutine has isolated heap - cannot share mutable object pointers.
 *
 * COPY KINDS:
 *   - IMMEDIATE: Primitives (int, float, bool, null) - no copy needed
 *   - SHARED: Immutables (strings) - pointer shared directly
 *   - DEEP: Mutables (array, map, closure) - recursive deep copy
 *   - SHARED_REF: shared objects - reference count incremented
 *
 * CYCLE DETECTION:
 *   Uses hash table (seen-table) to detect circular references.
 *   If object already copied, returns cached copy instead of infinite loop.
 *
 * CAUTION:
 *   - Closures capture upvalues by reference - deep copy creates new bindings
 *   - Large object graphs can be expensive - consider shared objects
 *
 * RELATED MODULES:
 *   - xcoro_heap.h: Target heap for copied objects
 *   - xshared.h: Reference counting for shared objects
 */

#ifndef XDEEP_COPY_H
#define XDEEP_COPY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../runtime/value/xvalue.h"

struct XrCoroutine;
struct XrGC;
struct XrayIsolate;

typedef enum {
    XR_COPY_IMMEDIATE = 0,
    XR_COPY_SHARED,
    XR_COPY_DEEP,
    XR_COPY_SHARED_REF
} XrCopyKind;

XR_FUNC XrCopyKind xr_value_copy_kind(XrValue value);

static inline bool xr_value_needs_copy(XrValue value) {
    return xr_value_copy_kind(value) == XR_COPY_DEEP;
}

XR_FUNC XrValue xr_deep_copy(struct XrayIsolate *X, XrValue value, struct XrGC *dst_gc);
XR_FUNC XrValue xr_deep_copy_counted(struct XrayIsolate *X, XrValue value, struct XrGC *dst_gc,
                                     int *out_count);
XR_FUNC XrValue xr_deep_copy_to_coro(struct XrayIsolate *X, XrValue value,
                                     struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_to_coro_counted(struct XrayIsolate *X, XrValue value,
                                             struct XrCoroutine *dst_coro, int *out_count);
XR_FUNC XrValue xr_deep_copy_array(struct XrayIsolate *X, struct XrArray *array,
                                   struct XrGC *dst_gc);
XR_FUNC XrValue xr_deep_copy_map(struct XrayIsolate *X, struct XrMap *map, struct XrGC *dst_gc);
XR_FUNC XrValue xr_deep_copy_closure(struct XrayIsolate *X, struct XrClosure *closure,
                                     struct XrGC *dst_gc);

typedef struct XrSeenEntry {
    void *src;
    XrValue dst;
    struct XrSeenEntry *next;
} XrSeenEntry;

// Arena block for bulk-allocating XrSeenEntry (avoids per-entry malloc)
#define XR_SEEN_ARENA_BLOCK_SIZE 64
typedef struct XrSeenArena {
    struct XrSeenArena *next;
    int used;
    XrSeenEntry entries[XR_SEEN_ARENA_BLOCK_SIZE];
} XrSeenArena;

struct XrCoroGC;

typedef struct XrCopyContext {
    struct XrayIsolate *X;
    struct XrGC *dst_gc;           // fixed GC fallback
    struct XrCoroGC *dst_coro_gc;  // Immix heap (preferred when non-NULL)
    XrSeenEntry **buckets;
    int bucket_count;
    int objects_copied;
    XrSeenArena *arena_head;  // arena block list for seen entries
} XrCopyContext;

XR_FUNC void xr_copy_context_init(XrCopyContext *ctx, struct XrayIsolate *X, struct XrGC *dst_gc);
XR_FUNC void xr_copy_context_cleanup(XrCopyContext *ctx);
XR_FUNC XrValue xr_deep_copy_with_ctx(XrCopyContext *ctx, XrValue value);

XR_FUNC bool xr_can_relocate(XrValue value);
XR_FUNC XrValue xr_to_shared(struct XrayIsolate *X, XrValue value);

/* ========== Per-Type Hooks Used by g_type_ops ==========
 *
 * One pair of callbacks per deep-copyable / shareable type, all sharing
 * the XrGCDeepCopyFn / XrGCToSharedFn signatures so g_type_ops can
 * dispatch directly without casts. The dispatchers above
 * (xr_deep_copy_with_ctx, xr_to_shared) consult these slots; any GC
 * type without a hook is simply not deep-copyable / not shareable
 * across coroutines and the dispatcher returns the value unchanged. */

#include "../runtime/gc/xgc_header.h"

XR_FUNC XrValue xr_deep_copy_array_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);
XR_FUNC XrValue xr_deep_copy_map_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);
XR_FUNC XrValue xr_deep_copy_set_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);
XR_FUNC XrValue xr_deep_copy_instance_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);
XR_FUNC XrValue xr_deep_copy_json_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);
XR_FUNC XrValue xr_deep_copy_closure_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);
XR_FUNC XrValue xr_deep_copy_datetime_with_ctx(struct XrCopyContext *ctx, struct XrGCHeader *obj);

XR_FUNC XrValue xr_to_shared_array(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_map(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_set(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_instance(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_json(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_closure(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_stringbuilder(struct XrayIsolate *X, struct XrGCHeader *obj);
XR_FUNC XrValue xr_to_shared_datetime(struct XrayIsolate *X, struct XrGCHeader *obj);

#endif  // XDEEP_COPY_H
