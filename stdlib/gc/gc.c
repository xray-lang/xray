/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * gc.c - GC control and monitoring module (reference-counting native API)
 *
 * KEY CONCEPT:
 *   Reclamation is per-coroutine reference counting; the only collection
 *   event is the Bacon-Rajan cycle collector. This module exposes an
 *   RC-native surface only:
 *   - gc.collect()                 - run cycle collection + whole-block reclaim
 *   - gc.disable() / gc.enable()   - pause/resume the automatic cycle collector
 *   - gc.isrunning()               - is automatic cycle collection enabled
 *   - gc.count() / gc.countb()     - live memory (KB / bytes)
 *   - gc.objects()                 - live object count
 *   - gc.cycles()                  - number of cycle collections run
 *   - gc.info()                    - introspection Map
 *
 *   Tracing-era knobs (step / debt / timems / state / fragmentation) are
 *   removed: they have no meaning under reference counting.
 */

#include "gc.h"
#include "../common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/gc/xcoro_heap.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/gc/xalloc_unified.h"
#include "../../src/base/xchecks.h"

/* ========== Helper ========== */

static XrCoroHeap *get_heap(XrayIsolate *isolate) {
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

/* ========== gc.collect() ========== */

// Run the cycle collector + whole-block reclaim on the current coroutine.
// Returns the cumulative cycle-collection count. Runs even when the
// automatic collector is disabled (explicit user request).
static XrValue gc_collect(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    if (gc) {
        xr_coro_heap_collect_cycles(gc);
        return xr_int((int64_t) gc->gc_count);
    }
    return xr_int(0);
}

/* ========== gc.count() ========== */

// Return live memory usage in KB
static XrValue gc_count(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    if (gc) {
        double kb = (double) gc->totalbytes / 1024.0;
        return xr_float(kb);
    }
    return xr_float(0.0);
}

/* ========== gc.countb() ========== */

// Return live memory usage in bytes
static XrValue gc_countb(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    return gc ? xr_int(gc->totalbytes) : xr_int(0);
}

/* ========== gc.disable() / gc.enable() ========== */

// Pause the automatic cycle collector (increment gc_disabled, saturate at 255).
// Under RC the only thing that can be paused is the cycle-collector
// auto-trigger; xr_cycle_add_root honours gc_disabled.
static XrValue gc_disable(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    if (gc && gc->gc_disabled < 255) {
        gc->gc_disabled++;
    }
    return xr_null();
}

// Resume the automatic cycle collector (decrement gc_disabled)
static XrValue gc_enable(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    if (gc && gc->gc_disabled > 0) {
        gc->gc_disabled--;
    }
    return xr_null();
}

/* ========== gc.isrunning() ========== */

// Whether the automatic cycle collector is enabled
static XrValue gc_isrunning(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    return xr_bool(gc && gc->gc_disabled == 0);
}

/* ========== gc.cycles() ========== */

// Return number of cycle collections run
static XrValue gc_cycles(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    return gc ? xr_int(gc->gc_count) : xr_int(0);
}

/* ========== gc.objects() ========== */

// Return total live GC object count (O(1) via incremental counter)
static XrValue gc_objects(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;
    XrCoroHeap *gc = get_heap(isolate);
    return gc ? xr_int((int64_t) gc->object_count) : xr_int(0);
}

/* ========== gc.info() ========== */

// Map keys in gc.info() use camelCase for every field.
#define MAP_SET(map, key_str, val) xr_map_set((map), xrs_string_value_c(isolate, (key_str)), (val))

// Return RC-native GC info as a Map
static XrValue gc_info(XrayIsolate *isolate, XrValue *args, int argc) {
    (void) argc;
    (void) args;

    XrCoroHeap *gc = get_heap(isolate);
    XrMap *map = xr_map_new(xr_current_coro(isolate));

    if (!gc) {
        MAP_SET(map, "error", xrs_string_value_c(isolate, "no gc"));
        return xr_value_from_map(map);
    }

    // Live memory + object stats
    MAP_SET(map, "totalBytes", xr_int(gc->totalbytes));
    MAP_SET(map, "totalKB", xr_float((double) gc->totalbytes / 1024.0));
    MAP_SET(map, "objects", xr_int((int64_t) gc->object_count));

    // Automatic cycle collector state
    MAP_SET(map, "running", xr_bool(gc->gc_disabled == 0));
    MAP_SET(map, "cycles", xr_int(gc->gc_count));
    MAP_SET(map, "finalizerCount", xr_int((int64_t) gc->finalizer_count));

    // Region block stats
    XrRegionStats istats;
    xr_region_get_stats(&gc->region, &istats);
    MAP_SET(map, "blocks", xr_int((int64_t) istats.total_blocks));
    MAP_SET(map, "freeBlocks", xr_int((int64_t) istats.free_blocks));
    MAP_SET(map, "fullBlocks", xr_int((int64_t) istats.full_blocks));

    return xr_value_from_map(map);
}

#undef MAP_SET

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module gc

XR_DEFINE_BUILTIN(gc_collect, "collect", "(): int",
                  "Run cycle collection + whole-block reclaim, return cycle count")
XR_DEFINE_BUILTIN(gc_disable, "disable", "(): ()", "Pause the automatic cycle collector")
XR_DEFINE_BUILTIN(gc_enable, "enable", "(): ()", "Resume the automatic cycle collector")
XR_DEFINE_BUILTIN(gc_isrunning, "isrunning", "(): bool",
                  "Check if automatic cycle collection is enabled")
XR_DEFINE_BUILTIN(gc_count, "count", "(): float", "Get live memory usage in KB")
XR_DEFINE_BUILTIN(gc_countb, "countb", "(): int", "Get live memory usage in bytes")
XR_DEFINE_BUILTIN(gc_objects, "objects", "(): int", "Get live GC object count")
XR_DEFINE_BUILTIN(gc_cycles, "cycles", "(): int", "Get number of cycle collections run")
XR_DEFINE_BUILTIN(gc_info, "info", "(): Map", "Get RC-native GC info as Map")

XR_FUNC XrModule *xr_load_module_gc(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_gc: NULL isolate");

    XrModule *module = xr_module_create_native(isolate, "gc");
    if (!module)
        return NULL;

    // Control
    XRS_EXPORT(module, isolate, "collect", gc_collect);
    XRS_EXPORT(module, isolate, "disable", gc_disable);
    XRS_EXPORT(module, isolate, "enable", gc_enable);
    XRS_EXPORT(module, isolate, "isrunning", gc_isrunning);

    // Statistics
    XRS_EXPORT(module, isolate, "count", gc_count);
    XRS_EXPORT(module, isolate, "countb", gc_countb);
    XRS_EXPORT(module, isolate, "objects", gc_objects);
    XRS_EXPORT(module, isolate, "cycles", gc_cycles);
    XRS_EXPORT(module, isolate, "info", gc_info);

    module->loaded = true;
    return module;
}
