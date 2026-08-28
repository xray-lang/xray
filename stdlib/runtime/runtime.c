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
 *   answers raw counters and applies no policy: which counters make up a
 *   snapshot and the derived kilobyte reading are the module body's, so both
 *   backends compile one statement of each.
 *
 *   Standalone AOT routes the same counters through the provider-owned current
 *   execution-local reclamation domain, so neither backend reports unrelated
 *   host-process state.
 */

#include "runtime.h"
#include "../common.h"
#include "../stdlib_cache.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/mem/xsystem_heap.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/base/xchecks.h"

/* ========== Helper ========== */

static XrCoroHeap *get_heap(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "get_heap: isolate must not be NULL");
    // Try current coroutine first (ensure heap exists via lazy init)
    XrCoroutine *coro = xr_current_coro(isolate);
    if (coro) {
        return xr_coro_ensure_heap(coro);
    }
    // Fallback to main coroutine
    coro = xr_isolate_get_main_coro(isolate);
    if (coro) {
        return xr_coro_ensure_heap(coro);
    }
    return NULL;
}

/* ========== runtime.__liveBytes() / runtime.__liveObjects() ========== */

// Live memory usage in bytes. Read on its own rather than through the snapshot
// so a monitoring loop costs a counter read and no allocation.
static XrValue runtime_live_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int(heap->totalbytes) : xr_int(0);
}

// Total live object count (O(1) via an incremental counter).
static XrValue runtime_live_objects(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int((int64_t) heap->object_count) : xr_int(0);
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

/* ========== runtime.__stats() ========== */

// One pass over the current execution-local reclamation domain. The record
// class is generated from stdlib/defs/core.def, so the runtime and the analyzer
// share one field schema. Callers select fields and derive scaled readings in
// runtime.xr; nothing here interprets the counters.
static XrValue runtime_stats(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;

    XrCoroHeap *heap = get_heap(isolate);
    XrClass *cls = xr_stdlib_record_class_get(isolate, "runtime", "__RuntimeStats");
    XR_CHECK(cls != NULL, "runtime.__stats: __RuntimeStats class unavailable");
    XrObjectInstance *object = xr_object_instance_new_with_class(xr_current_coro(isolate), cls);
    XR_CHECK(object != NULL, "runtime.__stats: __RuntimeStats allocation failed");

    XrRegionStats stats = {0};
    if (heap)
        xr_region_get_stats(&heap->region, &stats);

    xr_object_instance_set_by_key(isolate, object, "liveBytes",
                                  xr_int(heap ? heap->totalbytes : 0));
    xr_object_instance_set_by_key(isolate, object, "liveObjects",
                                  xr_int(heap ? (int64_t) heap->object_count : 0));
    xr_object_instance_set_by_key(isolate, object, "finalizerCount",
                                  xr_int(heap ? (int64_t) heap->finalizer_count : 0));
    xr_object_instance_set_by_key(isolate, object, "blocks", xr_int((int64_t) stats.total_blocks));
    xr_object_instance_set_by_key(isolate, object, "freeBlocks",
                                  xr_int((int64_t) stats.free_blocks));
    xr_object_instance_set_by_key(isolate, object, "fullBlocks",
                                  xr_int((int64_t) stats.full_blocks));

    return xr_object_instance_value(object);
}

#define XR_STDLIB_VM_BIND_MODULE_RUNTIME 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_RUNTIME

XR_FUNC XrModule *xr_native_module_create_runtime(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_native_module_create_runtime: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "runtime");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_runtime_generated(isolate, module);

    return module;
}
