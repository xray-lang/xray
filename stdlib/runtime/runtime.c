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
 *   Reclamation is per-coroutine reference counting; the only collection
 *   event is the Bacon-Rajan cycle collector. This module exposes the
 *   runtime control plane (task 154 moved it out of `mem`, which now only
 *   carries raw-memory capabilities):
 *   - runtime.collectCycles()            - run cycle collection + reclaim
 *   - runtime.disableCycleCollection()   - pause automatic cycle collection
 *   - runtime.enableCycleCollection()    - resume automatic cycle collection
 *   - runtime.isCycleCollectionEnabled() - automatic collector state
 *   - runtime.liveBytes()                - live memory bytes
 *   - runtime.liveObjects()              - live object count
 *   - runtime.info()                     - typed RuntimeInfo snapshot
 *
 *   Standalone AOT routes the same surface through the provider-owned current
 *   coroutine heap. The compiler materializes RuntimeInfo with the same typed
 *   field layout, so neither backend reports unrelated host-process state.
 */

#include "runtime.h"
#include "../common.h"
#include "../stdlib_cache.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/runtime/mem/xcoro_heap.h"
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

/* ========== runtime.collectCycles() ========== */

// Run the cycle collector + whole-block reclaim on the current coroutine.
// Returns the cumulative cycle-collection count. Runs even when the
// automatic collector is disabled (explicit user request).
static XrValue runtime_collect_cycles(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap) {
        xr_coro_heap_collect_cycles(heap);
        return xr_int((int64_t) heap->cycle_collect_count);
    }
    return xr_int(0);
}

/* ========== runtime.liveBytes() ========== */

// Return live memory usage in bytes
static XrValue runtime_live_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int(heap->totalbytes) : xr_int(0);
}

/* ========== runtime.disableCycleCollection() / runtime.enableCycleCollection() ========== */

// Pause the automatic cycle collector (increment cycle_collection_disabled, saturate at 255).
// Under RC the only thing that can be paused is the cycle-collector
// auto-trigger; xr_cycle_add_root honours cycle_collection_disabled.
static XrValue runtime_disable_cycle_collection(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap && heap->cycle_collection_disabled < 255) {
        heap->cycle_collection_disabled++;
    }
    return xr_null();
}

// Resume the automatic cycle collector (decrement cycle_collection_disabled)
static XrValue runtime_enable_cycle_collection(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap && heap->cycle_collection_disabled > 0) {
        heap->cycle_collection_disabled--;
    }
    return xr_null();
}

/* ========== runtime.isCycleCollectionEnabled() ========== */

// Whether the automatic cycle collector is enabled
static XrValue runtime_is_cycle_collection_enabled(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return xr_bool(heap && heap->cycle_collection_disabled == 0);
}

/* ========== runtime.liveObjects() ========== */

// Return total live object count (O(1) via incremental counter)
static XrValue runtime_live_objects(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int((int64_t) heap->object_count) : xr_int(0);
}

/* ========== runtime.info() ========== */

// Return a sealed, typed snapshot. The record class is generated from
// stdlib/defs/core.def, so the runtime and analyzer share one field schema.
static XrValue runtime_info(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;

    XrCoroHeap *heap = get_heap(isolate);
    XrClass *cls = xr_stdlib_record_class_get(isolate, "runtime", "RuntimeInfo");
    XR_CHECK(cls != NULL, "runtime.info: RuntimeInfo class unavailable");
    XrJson *record = xr_json_new_with_class(xr_current_coro(isolate), cls);
    XR_CHECK(record != NULL, "runtime.info: RuntimeInfo allocation failed");

    XrRegionStats stats = {0};
    if (heap)
        xr_region_get_stats(&heap->region, &stats);

    xr_json_set_by_key(isolate, record, "liveBytes", xr_int(heap ? heap->totalbytes : 0));
    xr_json_set_by_key(isolate, record, "liveKB",
                       xr_float(heap ? (double) heap->totalbytes / 1024.0 : 0.0));
    xr_json_set_by_key(isolate, record, "liveObjects",
                       xr_int(heap ? (int64_t) heap->object_count : 0));
    xr_json_set_by_key(isolate, record, "cycleCollectionEnabled",
                       xr_bool(heap && heap->cycle_collection_disabled == 0));
    xr_json_set_by_key(isolate, record, "cycleCollections",
                       xr_int(heap ? heap->cycle_collect_count : 0));
    xr_json_set_by_key(isolate, record, "finalizerCount",
                       xr_int(heap ? (int64_t) heap->finalizer_count : 0));
    xr_json_set_by_key(isolate, record, "blocks", xr_int((int64_t) stats.total_blocks));
    xr_json_set_by_key(isolate, record, "freeBlocks", xr_int((int64_t) stats.free_blocks));
    xr_json_set_by_key(isolate, record, "fullBlocks", xr_int((int64_t) stats.full_blocks));

    return xr_json_value(record);
}

#define XR_STDLIB_VM_BIND_MODULE_RUNTIME 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_RUNTIME

XR_FUNC XrModule *xr_load_module_runtime(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_runtime: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "runtime");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_runtime_generated(isolate, module);

    return module;
}
