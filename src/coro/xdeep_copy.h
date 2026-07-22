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
struct XrVMRuntime;

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

XR_FUNC XrValue xr_deep_copy(struct XrVMRuntime *X, XrValue value,
                             struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_counted(struct XrVMRuntime *X, XrValue value,
                                     struct XrFixedHeap *dst_fixed_heap, int *out_count);
XR_FUNC XrValue xr_deep_copy_to_coro(struct XrVMRuntime *X, XrValue value,
                                     struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_explicit_to_coro(struct XrVMRuntime *X, XrValue value,
                                              struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_explicit_to_storage(struct XrVMRuntime *X, XrValue value,
                                                 uint8_t storage_mode);
XR_FUNC XrValue xr_deep_copy_to_coro_counted(struct XrVMRuntime *X, XrValue value,
                                             struct XrCoroutine *dst_coro, int *out_count);
XR_FUNC XrValue xr_deep_copy_array(struct XrVMRuntime *X, struct XrArray *array,
                                   struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_map(struct XrVMRuntime *X, struct XrMap *map,
                                 struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_closure(struct XrVMRuntime *X, struct XrClosure *closure,
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

/* Recursion bound for the deep-copy walk (R2-6). The seen table makes cycles
 * safe, but acyclic chains miss it on every level and would otherwise recurse
 * until the C stack is exhausted (SIGSEGV). Aligned with the parser's
 * 1000-level nesting cap; deep_eq/json_serde use 256. When the bound is hit
 * the WHOLE copy fails (entry points return XR_NULL_VAL) — a truncated graph
 * is never handed back. */
#define XR_DEEP_COPY_MAX_DEPTH 1000

typedef struct XrCopyContext {
    struct XrRuntimeCore *core;
    struct XrFixedHeap *dst_fixed_heap;  // fixed heap fallback
    struct XrCoroHeap *dst_heap;         // Region heap (preferred when non-NULL)
    bool to_transit;                     // runtime temporary copy: sysheap + XR_OBJ_TRANSIT
    uint8_t dst_storage_mode;            // 0=normal, 1=shared system, 2=owned system
    bool share_existing_shared;          // boundary transfer may retain non-TRANSIT shared objs
    XrSeenEntry **buckets;
    int bucket_count;
    int objects_copied;
    XrSeenArena *arena_head;  // arena block list for seen entries
    int depth;                // current recursion depth (bounded by XR_DEEP_COPY_MAX_DEPTH)
    bool depth_exceeded;      // set once the bound is hit; whole copy fails
} XrCopyContext;

XR_FUNC void xr_copy_context_init_core(XrCopyContext *ctx, struct XrRuntimeCore *core,
                                       struct XrFixedHeap *dst_fixed_heap);
XR_FUNC void xr_copy_context_init(XrCopyContext *ctx, struct XrVMRuntime *X,
                                  struct XrFixedHeap *dst_fixed_heap);
XR_FUNC void xr_copy_context_cleanup(XrCopyContext *ctx);
XR_FUNC XrValue xr_deep_copy_with_ctx(XrCopyContext *ctx, XrValue value);

XR_FUNC XrValue xr_deep_copy_core(struct XrRuntimeCore *core, XrValue value,
                                  struct XrFixedHeap *dst_fixed_heap);
XR_FUNC XrValue xr_deep_copy_counted_core(struct XrRuntimeCore *core, XrValue value,
                                          struct XrFixedHeap *dst_fixed_heap, int *out_count);
XR_FUNC XrValue xr_deep_copy_to_coro_core(struct XrRuntimeCore *core, XrValue value,
                                          struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_explicit_to_coro_core(struct XrRuntimeCore *core, XrValue value,
                                                   struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_deep_copy_explicit_to_storage_core(struct XrRuntimeCore *core, XrValue value,
                                                      uint8_t storage_mode);
XR_FUNC XrValue xr_deep_copy_to_coro_counted_core(struct XrRuntimeCore *core, XrValue value,
                                                  struct XrCoroutine *dst_coro, int *out_count);

/* ========== Residual Transit Copies ==========
 *
 * Channel/Task payloads are materialized as owned message roots and do not
 * consume TRANSIT. TRANSIT remains only for non-Channel runtime aggregation
 * paths that still need a coroutine-independent temporary graph. The consumer
 * deep-copies the graph into its private heap and releases the transit
 * reference, freeing the graph through the regular shared-destroy path. */

XR_FUNC XrValue xr_deep_copy_to_transit_core(struct XrRuntimeCore *core, XrValue value);
XR_FUNC XrValue xr_deep_copy_to_transit(struct XrVMRuntime *X, XrValue value);
XR_FUNC void xr_chan_transit_release_core(struct XrRuntimeCore *core, XrValue value);

/* ========== Per-Type Explicit-Copy Hooks ========== */

#include "../runtime/mem/xobj_header.h"

XR_FUNC XrValue xr_deep_copy_array_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_string_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_map_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_set_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_instance_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_closure_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_cell_with_ctx(struct XrCopyContext *ctx, struct XrObjHeader *obj);
XR_FUNC XrValue xr_deep_copy_enum_descriptor_with_ctx(struct XrCopyContext *ctx,
                                                      struct XrObjHeader *obj);

#endif  // XDEEP_COPY_H
