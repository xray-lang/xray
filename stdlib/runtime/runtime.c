/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * runtime.c - Xray runtime introspection/control module
 *
 * KEY CONCEPT:
 *   Reclamation is per-coroutine reference counting and nothing else — there
 *   is no collection event to observe or control, which is why this module
 *   has no collect/enable/disable entry point; those controls were deleted with
 *   the collector rather than retained as no-op stubs. This module exposes the
 *   runtime introspection surface, while `mem` only carries raw-memory
 *   capabilities:
 *   The public surface lives in stdlib/runtime/runtime.xr. What remains here
 *   answers raw scalar counters and applies no policy: which counters make up
 *   a view and the derived kilobyte reading are the module body's, so both
 *   backends compile one statement of each.
 *
 *   Standalone AOT routes the same counters through the provider-owned current
 *   execution-local reclamation domain, so neither backend reports unrelated
 *   host-process state.
 */

#include "../common.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/mem/xsystem_heap.h"

/* ========== runtime.__liveBytes() / runtime.__liveObjects() ========== */

// Live memory usage in bytes. Read on its own rather than through the snapshot
// so a monitoring loop costs a counter read and no allocation.
static XrValue runtime_live_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = xr_isolate_get_heap(isolate);
    return heap ? xr_int(heap->totalbytes) : xr_int(0);
}

// Total live object count (O(1) via an incremental counter).
static XrValue runtime_live_objects(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = xr_isolate_get_heap(isolate);
    return heap ? xr_int((int64_t) heap->object_count) : xr_int(0);
}

// Registered finalizers in the current execution-local reclamation domain.
static XrValue runtime_finalizer_count(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = xr_isolate_get_heap(isolate);
    return heap ? xr_int((int64_t) heap->finalizer_count) : xr_int(0);
}

/* ========== runtime.__sharedLiveBytes() / runtime.__staticAllocBytes() ========== */

// Live bytes held by SYNC_SHARED system-heap objects. The reclamation-domain
// counters below read the current coroutine heap; this reads the one domain a
// coroutine's teardown never bounds.
static XrValue runtime_shared_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) argc;
    (void) args;
    return xr_int((int64_t) xr_sysheap_shared_live_bytes_total());
}

// Memory allocated into the MODULE_STATIC class/module arena (grows only).
static XrValue runtime_static_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    (void) argc;
    (void) args;
    return xr_int((int64_t) xr_sysheap_static_alloc_bytes_total());
}

#define XR_STDLIB_VM_BIND_MODULE_RUNTIME 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_RUNTIME
