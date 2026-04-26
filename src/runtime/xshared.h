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

/* ========== Shared Reference Count Operations ========== */

// Refcount stored in gc_next pointer (shared objects are on system heap,
// not in GC linked lists, so gc_next is unused).
// This preserves objsize for correct munmap on mmap-allocated objects.
static inline _Atomic(uintptr_t) *xr_shared_refc_ptr(XrGCHeader *gc) {
    return (_Atomic(uintptr_t) *) &gc->gc_next;
}

static inline int xr_shared_get_refc(XrGCHeader *gc) {
    return (int) atomic_load(xr_shared_refc_ptr(gc));
}

static inline void xr_shared_set_refc(XrGCHeader *gc, int refc) {
    atomic_store(xr_shared_refc_ptr(gc), (uintptr_t) refc);
}

static inline int xr_shared_incref(XrGCHeader *gc) {
    return (int) atomic_fetch_add(xr_shared_refc_ptr(gc), 1) + 1;
}

static inline int xr_shared_decref(XrGCHeader *gc) {
    uintptr_t old = atomic_fetch_sub(xr_shared_refc_ptr(gc), 1);
    return (old <= 1) ? 0 : (int) (old - 1);
}

static inline void xr_shared_init(XrGCHeader *gc) {
    XR_GC_SET_STORAGE(gc, XR_GC_STORAGE_SHARED);
    xr_shared_set_refc(gc, 1);
}

/* ========== Shared Object Destruction ========== */

// Destroy shared object: call destructor then free memory
// Must be called when refcount reaches 0
XR_FUNC void xr_shared_destroy(XrGCHeader *obj);

#endif  // XSHARED_H
