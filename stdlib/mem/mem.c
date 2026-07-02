/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * mem.c - RC memory and cycle-collection introspection module
 *
 * KEY CONCEPT:
 *   Reclamation is per-coroutine reference counting; the only collection
 *   event is the Bacon-Rajan cycle collector. This module exposes a
 *   memory-model-native surface only:
 *   - mem.collectCycles()                 - run cycle collection + reclaim
 *   - mem.disableCycleCollection()        - pause automatic cycle collection
 *   - mem.enableCycleCollection()         - resume automatic cycle collection
 *   - mem.isCycleCollectionEnabled()      - automatic collector state
 *   - mem.liveBytes()                     - live memory bytes
 *   - mem.liveObjects()                   - live object count
 *   - mem.info()                          - introspection Map
 *
 *   Tracing-era knobs (step / debt / timems / state / fragmentation) are
 *   removed: they have no meaning under reference counting.
 */

#include "mem.h"
#include "../common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/mem/xcoro_heap.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/mem/xalloc_unified.h"
#include "../../src/shared/xr_bits_core.h"
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

/* ========== mem.collectCycles() ========== */

// Run the cycle collector + whole-block reclaim on the current coroutine.
// Returns the cumulative cycle-collection count. Runs even when the
// automatic collector is disabled (explicit user request).
static XrValue mem_collect_cycles(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap) {
        xr_coro_heap_collect_cycles(heap);
        return xr_int((int64_t) heap->cycle_collect_count);
    }
    return xr_int(0);
}

/* ========== mem.liveBytes() ========== */

// Return live memory usage in bytes
static XrValue mem_live_bytes(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int(heap->totalbytes) : xr_int(0);
}

/* ========== mem.disableCycleCollection() / mem.enableCycleCollection() ========== */

// Pause the automatic cycle collector (increment cycle_collection_disabled, saturate at 255).
// Under RC the only thing that can be paused is the cycle-collector
// auto-trigger; xr_cycle_add_root honours cycle_collection_disabled.
static XrValue mem_disable_cycle_collection(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap && heap->cycle_collection_disabled < 255) {
        heap->cycle_collection_disabled++;
    }
    return xr_null();
}

// Resume the automatic cycle collector (decrement cycle_collection_disabled)
static XrValue mem_enable_cycle_collection(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    if (heap && heap->cycle_collection_disabled > 0) {
        heap->cycle_collection_disabled--;
    }
    return xr_null();
}

/* ========== mem.isCycleCollectionEnabled() ========== */

// Whether the automatic cycle collector is enabled
static XrValue mem_is_cycle_collection_enabled(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return xr_bool(heap && heap->cycle_collection_disabled == 0);
}

/* ========== mem.liveObjects() ========== */

// Return total live object count (O(1) via incremental counter)
static XrValue mem_live_objects(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *heap = get_heap(isolate);
    return heap ? xr_int((int64_t) heap->object_count) : xr_int(0);
}

/* ========== mem.info() ========== */

// Map keys in mem.info() use camelCase for every field.
#define MAP_SET(map, key_str, val) xr_map_set((map), xrs_string_value_c(isolate, (key_str)), (val))

// Return memory-model-native info as a Map
static XrValue mem_info(XrVMRuntime *isolate, XrValue *args, int argc) {
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

/* ========== Bit intrinsics (mem.popcount / leadingZeros / ...) ========== */

/*
 * Pure 64-bit bit-manipulation intrinsics. Semantics live in the shared
 * core (src/shared/xr_bits_core.h) so the VM bindings here and the AOT
 * wrappers in src/aot/xrt_mem.h stay bit-identical. No heap / coroutine
 * state is touched, so these are safe on any thread.
 */

static XrValue mem_popcount(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(0);
    return xr_int(xr_bits_core_popcount(XR_TO_INT(args[0])));
}

static XrValue mem_leading_zeros(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(64);
    return xr_int(xr_bits_core_leading_zeros(XR_TO_INT(args[0])));
}

static XrValue mem_trailing_zeros(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(64);
    return xr_int(xr_bits_core_trailing_zeros(XR_TO_INT(args[0])));
}

static XrValue mem_byteswap(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 1 || !XR_IS_INT(args[0]))
        return xr_int(0);
    return xr_int(xr_bits_core_byteswap(XR_TO_INT(args[0])));
}

static XrValue mem_rotate_left(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_bits_core_rotate_left(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

static XrValue mem_rotate_right(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) isolate;
    if (argc < 2 || !XR_IS_INT(args[0]) || !XR_IS_INT(args[1]))
        return xr_int(0);
    return xr_int(xr_bits_core_rotate_right(XR_TO_INT(args[0]), XR_TO_INT(args[1])));
}

/* ========== Module Loading ========== */

#define XR_STDLIB_VM_BIND_MODULE_MEM 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_MEM

XR_FUNC XrModule *xr_load_module_mem(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_mem: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "mem");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_mem_generated(isolate, module);

    module->loaded = true;
    return module;
}
