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
 *   - runtime.info()                     - introspection Map
 *
 *   All functions are VM-only introspection (aot_direct: false, no aot
 *   helper) — same as before the move: the AOT cgen rejects them with an
 *   explicit "unsupported" error rather than silently misbehaving. Giving
 *   standalone AOT binaries a real introspection surface is a separate,
 *   additive task.
 */

#include "runtime.h"
#include "../common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/object/xmap.h"
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

// Map keys in runtime.info() use camelCase for every field.
#define MAP_SET(map, key_str, val) xr_map_set((map), xrs_string_value_c(isolate, (key_str)), (val))

// Return memory-model-native info as a Map
static XrValue runtime_info(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;

    XrCoroHeap *heap = get_heap(isolate);
    XrMap *map = xr_map_new(xr_current_coro(isolate));

    if (!heap) {
        MAP_SET(map, "error", xrs_string_value_c(isolate, "no memory heap"));
        return xr_value_from_map(map);
    }

    // Live memory + object stats
    MAP_SET(map, "liveBytes", xr_int(heap->totalbytes));
    MAP_SET(map, "liveKB", xr_float((double) heap->totalbytes / 1024.0));
    MAP_SET(map, "liveObjects", xr_int((int64_t) heap->object_count));

    // Automatic cycle collector state
    MAP_SET(map, "cycleCollectionEnabled", xr_bool(heap->cycle_collection_disabled == 0));
    MAP_SET(map, "cycleCollections", xr_int(heap->cycle_collect_count));
    MAP_SET(map, "finalizerCount", xr_int((int64_t) heap->finalizer_count));

    // Region block stats
    XrRegionStats istats;
    xr_region_get_stats(&heap->region, &istats);
    MAP_SET(map, "blocks", xr_int((int64_t) istats.total_blocks));
    MAP_SET(map, "freeBlocks", xr_int((int64_t) istats.free_blocks));
    MAP_SET(map, "fullBlocks", xr_int((int64_t) istats.full_blocks));

    return xr_value_from_map(map);
}

#undef MAP_SET

#define XR_STDLIB_VM_BIND_MODULE_RUNTIME 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_RUNTIME

XR_FUNC XrModule *xr_load_module_runtime(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_runtime: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "runtime");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_runtime_generated(isolate, module);

    module->loaded = true;
    return module;
}
