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
struct XrFixedHeap;
struct XrRuntimeCore;
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

XR_FUNC XrValue xr_deep_copy(struct XrayIsolate *X, XrValue value,
                             struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_counted(struct XrayIsolate *X, XrValue value,
                                     struct XrFixedHeap *dst_fixed_heap, int *out_count);
XR_FUNC XrValue xr_deep_copy_to_coro(struct XrayIsolate *X, XrValue value,
                                     struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_to_coro_counted(struct XrayIsolate *X, XrValue value,
                                             struct XrCoroutine *dst_coro, int *out_count);
XR_FUNC XrValue xr_deep_copy_array(struct XrayIsolate *X, struct XrArray *array,
                                   struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_map(struct XrayIsolate *X, struct XrMap *map,
                                 struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_closure(struct XrayIsolate *X, struct XrClosure *closure,
                                     struct XrFixedHeap *dst_fixed_heap);

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

struct XrCoroHeap;

typedef struct XrCopyContext {
    struct XrRuntimeCore *core;
    struct XrFixedHeap *dst_fixed_heap;  // fixed heap fallback
    struct XrCoroHeap *dst_heap;         // Region heap (preferred when non-NULL)
    bool to_transit;                     // channel-transit copy: sysheap + XR_OBJ_TRANSIT
    XrSeenEntry **buckets;
    int bucket_count;
    int objects_copied;
    XrSeenArena *arena_head;  // arena block list for seen entries
} XrCopyContext;

XR_FUNC void xr_copy_context_init_core(XrCopyContext *ctx, struct XrRuntimeCore *core,
                                       struct XrFixedHeap *dst_fixed_heap);
XR_FUNC void xr_copy_context_init(XrCopyContext *ctx, struct XrayIsolate *X,
                                  struct XrFixedHeap *dst_fixed_heap);
XR_FUNC void xr_copy_context_cleanup(XrCopyContext *ctx);
XR_FUNC XrValue xr_deep_copy_with_ctx(XrCopyContext *ctx, XrValue value);

XR_FUNC XrValue xr_deep_copy_core(struct XrRuntimeCore *core, XrValue value,
                                  struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_counted_core(struct XrRuntimeCore *core, XrValue value,
                                          struct XrFixedHeap *dst_fixed_heap, int *out_count);
XR_FUNC XrValue xr_deep_copy_to_coro_core(struct XrRuntimeCore *core, XrValue value,
                                          struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_to_coro_counted_core(struct XrRuntimeCore *core, XrValue value,
                                                  struct XrCoroutine *dst_coro, int *out_count);

/* ========== Channel Transit Copies ==========
 *
 * A value sent through a channel must outlive the sending coroutine, so
 * the send side deep-copies it into a coroutine-independent TRANSIT graph
 * (atomic refcount, one reference owned by the channel buffer / blocked
 * sender). The receive side deep-copies the graph into the receiving
 * coroutine's heap and releases the buffer reference, which frees the
 * whole transit graph through the regular shared-destroy path. */

XR_FUNC XrValue xr_deep_copy_to_transit_core(struct XrRuntimeCore *core, XrValue value);
XR_FUNC XrValue xr_deep_copy_to_transit(struct XrayIsolate *X, XrValue value);
XR_FUNC void xr_chan_transit_release_core(struct XrRuntimeCore *core, XrValue value);

/* ========== Zero-copy buffer move for self-contained scalar arrays ==========
 *
 * A typed (non-ANY) array's data buffer holds only scalars — no interior
 * pointers into any coroutine heap — so when the sender holds the unique
 * reference (move semantics) the buffer can be re-homed across heaps instead
 * of malloc+memcpy'd twice (send→transit, transit→recv). The small XrArray
 * struct is still allocated per side; only the (potentially large) data
 * buffer pointer moves.
 *
 * Preconditions are re-checked inside each helper; both fall back (return
 * false) to the normal deep-copy path for anything not provably safe to move
 * (ANY arrays, slices, Region-blob-backed data, aliased/non-unique refs).
 *
 * xr_chan_try_move_array_to_transit: send side. Caller must already own the
 *   last reference (it has done the drop). On success *out is a TRANSIT array
 *   that stole the source buffer; the now-empty source struct is the caller's
 *   to free.
 * xr_chan_try_adopt_array_from_transit: recv side. On success *out is a
 *   receiver-heap array that stole the transit buffer, and the emptied transit
 *   struct has been released. */
XR_FUNC bool xr_chan_try_move_array_to_transit_core(struct XrRuntimeCore *core, XrValue value,
                                                    XrValue *out);
XR_FUNC bool xr_chan_try_move_array_to_transit(struct XrayIsolate *X, XrValue value, XrValue *out);
XR_FUNC bool xr_chan_try_adopt_array_from_transit_core(XrValue value, struct XrCoroutine *recv_coro,
                                                       XrValue *out);
XR_FUNC bool xr_chan_try_adopt_array_from_transit(struct XrayIsolate *X, XrValue value,
                                                  struct XrCoroutine *recv_coro, XrValue *out);

XR_FUNC bool xr_can_relocate(XrValue value);
XR_FUNC XrValue xr_to_shared(struct XrayIsolate *X, XrValue value);

/* ========== Per-Type Transfer Hooks ==========
 *
 * One pair of callbacks per deep-copyable / shareable type, all sharing
 * the XrObjDeepCopyFn / XrObjToSharedFn signatures so the split transfer
 * tables can dispatch directly without casts. The dispatchers above
 * (xr_deep_copy_with_ctx, xr_to_shared) consult these slots; any object
 * type without a hook is simply not deep-copyable / not shareable
 * across coroutines and the dispatcher returns the value unchanged. */

#include "../runtime/gc/xgc_header.h"

XR_FUNC XrValue xr_deep_copy_array_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_map_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_set_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_instance_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_closure_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_cell_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);

XR_FUNC XrValue xr_to_shared_array(struct XrayIsolate *X, struct XrObjHeader *obj);
XR_FUNC XrValue xr_to_shared_map(struct XrayIsolate *X, struct XrObjHeader *obj);
XR_FUNC XrValue xr_to_shared_set(struct XrayIsolate *X, struct XrObjHeader *obj);
XR_FUNC XrValue xr_to_shared_instance(struct XrayIsolate *X, struct XrObjHeader *obj);
XR_FUNC XrValue xr_to_shared_closure(struct XrayIsolate *X, struct XrObjHeader *obj);

#endif  // XDEEP_COPY_H
