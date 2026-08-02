/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xatomic_core.c - VM-neutral Atomic<T> storage and operations
 */

#include "xatomic.h"

#include "../core/xr_runtime_core.h"
#include "../mem/xsystem_heap.h"
#include "../xisolate_api.h"
#include "../xshared.h"
#include "../../base/xchecks.h"

XrAtomic *xr_atomic_new_core(XrRuntimeCore *core, XrAtomicKind kind, int64_t initial) {
    XR_DCHECK(core != NULL, "xr_atomic_new_core: NULL runtime core");
    XR_DCHECK(kind <= XR_ATOMIC_BOOL, "xr_atomic_new_core: invalid kind");

    XrSystemHeap *heap = core ? core->sys_heap : NULL;
    if (!heap)
        return NULL;

    XrAtomic *a = (XrAtomic *) xr_sysheap_alloc_shared(heap, sizeof(XrAtomic), XR_TATOMIC);
    if (!a)
        return NULL;

    /* Atomic shared-RC: pure cross-coroutine shared data, no executor owner, so
     * the compiler tracks it like `shared` (dup = atomic incref, last drop
     * frees). NOT XR_OBJ_MANAGED — that would leak the handle (drop no-op). */
    xr_shared_set_refc(&a->hdr, 1);
    a->kind = (uint8_t) kind;
    atomic_store(&a->value, initial);
    return a;
}

XrAtomic *xr_atomic_new(XrVMRuntime *X, XrAtomicKind kind, int64_t initial) {
    XR_DCHECK(X != NULL, "xr_atomic_new: NULL isolate");
    return xr_atomic_new_core(xr_isolate_get_runtime_core(X), kind, initial);
}

int64_t xr_atomic_load(XrAtomic *a, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_load: NULL atomic");
    return atomic_load_explicit(&a->value, xr_to_c11_load_order(ord));
}

void xr_atomic_store(XrAtomic *a, int64_t val, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_store: NULL atomic");
    atomic_store_explicit(&a->value, val, xr_to_c11_store_order(ord));
}

int64_t xr_atomic_fetch_add(XrAtomic *a, int64_t delta, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_fetch_add: NULL atomic");
    return atomic_fetch_add_explicit(&a->value, delta, xr_to_c11_rmw_order(ord));
}

int64_t xr_atomic_fetch_sub(XrAtomic *a, int64_t delta, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_fetch_sub: NULL atomic");
    return atomic_fetch_sub_explicit(&a->value, delta, xr_to_c11_rmw_order(ord));
}

int64_t xr_atomic_swap(XrAtomic *a, int64_t desired, XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_swap: NULL atomic");
    return atomic_exchange_explicit(&a->value, desired, xr_to_c11_rmw_order(ord));
}

bool xr_atomic_compare_exchange(XrAtomic *a, int64_t *expected, int64_t desired,
                                XrAtomicOrdering ord) {
    XR_DCHECK(a != NULL, "xr_atomic_compare_exchange: NULL atomic");
    XR_DCHECK(expected != NULL, "xr_atomic_compare_exchange: NULL expected");
    /* Strong CAS: no spurious failures. On failure, *expected is
     * updated to the current value (standard C11 behaviour). */
    return atomic_compare_exchange_strong_explicit(&a->value, expected, desired,
                                                   xr_to_c11_rmw_order(ord), memory_order_relaxed);
}
