/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xshared.h - Reference counting for shared objects
 *
 * KEY CONCEPT:
 *   Shared objects live in global heap with atomic refcount.
 *   Support concurrent access from multiple coroutines.
 *
 * SHARED VARIABLE TYPES:
 *   shared const x = value
 *     - Atomic refcount, all coroutines can read concurrently
 *     - Immutable after creation, zero-copy sharing
 *     - Example: shared const config = { port: 8080 }
 *
 *   shared let x = value
 *     - Only accessible via Channel for serialized read/write
 *     - Must send through channel, cannot be read directly
 *     - Example: shared let counter = 0; ch.send(counter)
 *
 * SCOPING RULES:
 *   - Stored in global heap, but visibility is lexical scope only
 *   - Unlike global variables, shared variables respect block scope
 *   - Duplicate shared variable names across scopes cause compile error
 *   - This prevents accidental shadowing and name conflicts
 */

#ifndef XSHARED_H
#define XSHARED_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "gc/xgc_header.h"

struct XrRuntimeCore;

/* ========== Shared Reference Count Operations ==========
 *
 * Shared (cross-coroutine) objects use the same sign-tagged refcount field
 * as the per-coroutine RC, in the atomic (negative) band: a live count of N
 * is stored as -N, so the compiler's hot-path sign test routes every shared
 * object to its cold/atomic path automatically (see xgc_header.h). These
 * helpers present a conventional positive count to the runtime while doing
 * the atomic arithmetic on the negative encoding. */

static inline _Atomic(int32_t) *xr_shared_refc_ptr(XrGCHeader *gc) {
    /* refcount is declared _Atomic in the header; no cast needed. */
    return &gc->refcount;
}

static inline int xr_shared_get_refc(XrGCHeader *gc) {
    return (int) (-atomic_load(xr_shared_refc_ptr(gc)));
}

static inline void xr_shared_set_refc(XrGCHeader *gc, int refc) {
    XR_OBJ_SET_FLAG(gc, XR_OBJ_ATOMIC);
    /* Negative encoding: references = -rc, so N refs are stored as -N. */
    atomic_store(xr_shared_refc_ptr(gc), (int32_t) (-refc));
}

static inline int xr_shared_incref(XrGCHeader *gc) {
    /* More references = more negative. Returns the new (positive) count. */
    int32_t old = atomic_fetch_sub(xr_shared_refc_ptr(gc), 1);
    return (int) (-(old - 1));
}

static inline int xr_shared_decref(XrGCHeader *gc) {
    /* Releasing moves toward zero. old == -1 means this was the last
     * reference (count drops to 0). Returns the new (positive) count. */
    int32_t old = atomic_fetch_add(xr_shared_refc_ptr(gc), 1);
    return (old == -1) ? 0 : (int) (-(old + 1));
}

static inline void xr_shared_init(XrGCHeader *gc) {
    XR_GC_SET_STORAGE(gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(gc, 1);
}

/* ========== Shared Object Destruction ========== */

// Destroy shared object: call destructor then free memory.
// Must be called when refcount reaches 0. The runtime core owns the destroy
// capability table for the object type.
XR_FUNC void xr_shared_destroy_core(struct XrRuntimeCore *core, XrGCHeader *obj);

#endif  // XSHARED_H
