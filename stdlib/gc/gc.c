/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * gc.c - GC control and monitoring module
 *
 * KEY CONCEPT:
 *   Per-coroutine GC control and statistics:
 *   - gc.collect() - Force full GC cycle
 *   - gc.count() / gc.countb() - Memory statistics
 *   - gc.setpause(n) / gc.setstepmul(n) - Tune GC parameters
 *   - gc.info() - Full GC status including generational info
 */

#include "gc.h"
#include "../../src/module/xmodule.h"
#include "../../src/runtime/gc/xcoro_gc.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/xexec_frame.h"
#include "../../src/coro/xcoroutine.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/gc/xalloc_unified.h"

// Forward declarations for native module registration
extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);

/* ========== Helper ========== */

static XrCoroGC* get_coro_gc(XrayIsolate *isolate) {
    // Try current coroutine first (ensure coro_gc exists via lazy init)
    XrCoroutine *coro = xr_current_coro(isolate);
    if (coro) {
        return xr_coro_ensure_gc(coro);
    }
    // Fallback to main coroutine
    coro = xr_isolate_get_main_coro(isolate);
    if (coro) {
        return xr_coro_ensure_gc(coro);
    }
    return NULL;
}

/* ========== gc.collect() ========== */

// Force a full GC cycle on current coroutine
static XrValue gc_collect(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (gc) {
        xr_coro_gc_fullgc(gc);
        return xr_int((int64_t)gc->gc_count);
    }
    return xr_int(0);
}

/* ========== gc.step() ========== */

// Execute one incremental GC step
static XrValue gc_step(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (gc) {
        xr_coro_gc_step(gc);
        return xr_bool(gc->gcstate == XGC_PAUSE);
    }
    return xr_bool(true);
}

/* ========== gc.count() ========== */

// Return memory usage in KB
static XrValue gc_count(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (gc) {
        double kb = (double)gc->totalbytes / 1024.0;
        return xr_float(kb);
    }
    return xr_float(0.0);
}

/* ========== gc.countb() ========== */

// Return memory usage in bytes
static XrValue gc_countb(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    return gc ? xr_int(gc->totalbytes) : xr_int(0);
}

/* ========== gc.disable() / gc.enable() ========== */

// Disable GC (increment gc_disabled counter, saturate at 255)
static XrValue gc_disable(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (gc && gc->gc_disabled < 255) {
        gc->gc_disabled++;
    }
    return xr_null();
}

// Enable GC (decrement gc_disabled counter)
static XrValue gc_enable(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (gc && gc->gc_disabled > 0) {
        gc->gc_disabled--;
    }
    return xr_null();
}

/* ========== gc.isrunning() ========== */

// Check if GC is enabled
static XrValue gc_isrunning(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    return xr_bool(gc && gc->gc_disabled == 0);
}

/* ========== gc.setpause(n) / gc.setstepmul(n) ========== */

// Set GC pause multiplier, returns old value
static XrValue gc_setpause(XrayIsolate *isolate, XrValue *args, int argc) {
    XrCoroGC *gc = get_coro_gc(isolate);
    if (!gc) return xr_int(0);
    
    int old = gc->gc_pause;
    if (argc > 0 && XR_IS_INT(args[0])) {
        int newval = (int)XR_TO_INT(args[0]);
        if (newval > 0) gc->gc_pause = newval;
    }
    return xr_int(old);
}

// Set GC step multiplier, returns old value
static XrValue gc_setstepmul(XrayIsolate *isolate, XrValue *args, int argc) {
    XrCoroGC *gc = get_coro_gc(isolate);
    if (!gc) return xr_int(0);
    
    int old = gc->gc_stepmul;
    if (argc > 0 && XR_IS_INT(args[0])) {
        int newval = (int)XR_TO_INT(args[0]);
        if (newval > 0) gc->gc_stepmul = newval;
    }
    return xr_int(old);
}

/* ========== gc.gccount() ========== */

// Return number of GC cycles completed
static XrValue gc_gccount(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    return gc ? xr_int(gc->gc_count) : xr_int(0);
}

/* ========== gc.objects() ========== */

// Return total live GC object count (O(1) via incremental counter)
static XrValue gc_objects(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    return gc ? xr_int((int64_t)gc->object_count) : xr_int(0);
}

/* ========== gc.debt() ========== */

// Return current GC debt
static XrValue gc_debt(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    return gc ? xr_int(gc->GCdebt) : xr_int(0);
}

/* ========== gc.state() ========== */

// Use xr_gc_state_name() from xcoro_gc.h
#define state_name(s) xr_gc_state_name(s)

// Return current GC state as string
static XrValue gc_state(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    const char *name = gc ? state_name(gc->gcstate) : "NONE";
    XrString *str = xr_string_intern(isolate, name, strlen(name), 0);
    return xr_string_value(str);
}

/* ========== gc.timems() ========== */

// Return cumulative GC time in milliseconds
static XrValue gc_timems(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (!gc) return xr_float(0.0);
    return xr_float((double)gc->gc_time_ns / 1e6);
}

/* ========== gc.fragmentation() ========== */

// Return heap fragmentation ratio (0.0 = compact, 1.0 = fully fragmented)
static XrValue gc_fragmentation(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    XrCoroGC *gc = get_coro_gc(isolate);
    if (!gc) return xr_float(0.0);
    
    XrImmixStats stats;
    xr_immix_get_stats(&gc->immix, &stats);
    
    size_t total = stats.live_lines + stats.free_lines;
    if (total == 0) return xr_float(0.0);
    return xr_float(1.0 - (double)stats.live_lines / (double)total);
}

/* ========== gc.info() ========== */

#define MAP_SET(map, key_str, val) do { \
    XrString *_k = xr_string_intern(isolate, key_str, strlen(key_str), 0); \
    xr_map_set(map, xr_string_value(_k), val); \
} while(0)

// Return comprehensive GC info as a Map
static XrValue gc_info(XrayIsolate *isolate, XrValue *args, int argc) {
    (void)argc; (void)args;
    
    XrCoroGC *gc = get_coro_gc(isolate);
    XrMap *map = xr_map_new(xr_current_coro(isolate));
    
    if (!gc) {
        MAP_SET(map, "error", xr_string_value(xr_string_intern(isolate, "no gc", 5, 0)));
        return xr_value_from_map(map);
    }
    
    // Memory stats
    MAP_SET(map, "totalbytes", xr_int(gc->totalbytes));
    MAP_SET(map, "totalKB", xr_float((double)gc->totalbytes / 1024.0));
    MAP_SET(map, "marked", xr_int(gc->GCmarked));
    MAP_SET(map, "debt", xr_int(gc->GCdebt));
    
    // GC state
    const char *sname = state_name(gc->gcstate);
    MAP_SET(map, "state", xr_string_value(xr_string_intern(isolate, sname, strlen(sname), 0)));
    MAP_SET(map, "running", xr_bool(gc->gc_disabled == 0));
    MAP_SET(map, "gccount", xr_int(gc->gc_count));
    
    // Tuning parameters
    MAP_SET(map, "pause", xr_int(gc->gc_pause));
    MAP_SET(map, "stepmul", xr_int(gc->gc_stepmul));
    
    // Timing stats
    MAP_SET(map, "gctimeMs", xr_float((double)gc->gc_time_ns / 1e6));
    MAP_SET(map, "lastgctimeUs", xr_float((double)gc->last_gc_time_ns / 1e3));
    
    // Monitoring stats
    MAP_SET(map, "finalizercount", xr_int((int64_t)gc->finalizer_count));
    
    // Immix block/line stats
    XrImmixStats istats;
    xr_immix_get_stats(&gc->immix, &istats);
    MAP_SET(map, "blocks", xr_int((int64_t)istats.total_blocks));
    MAP_SET(map, "freeblocks", xr_int((int64_t)istats.free_blocks));
    MAP_SET(map, "recycleblocks", xr_int((int64_t)istats.recycle_blocks));
    MAP_SET(map, "fullblocks", xr_int((int64_t)istats.full_blocks));
    MAP_SET(map, "livelines", xr_int((int64_t)istats.live_lines));
    MAP_SET(map, "freelines", xr_int((int64_t)istats.free_lines));
    
    return xr_value_from_map(map);
}

#undef MAP_SET

/* ========== Module Loading ========== */

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module gc

XR_DEFINE_BUILTIN(gc_collect, "collect", "(): int", "Force full GC, return cycle count")
XR_DEFINE_BUILTIN(gc_step, "step", "(): bool", "Run one incremental GC step, return true if cycle completed")
XR_DEFINE_BUILTIN(gc_disable, "disable", "(): void", "Disable automatic GC")
XR_DEFINE_BUILTIN(gc_enable, "enable", "(): void", "Enable automatic GC")
XR_DEFINE_BUILTIN(gc_isrunning, "isrunning", "(): bool", "Check if GC is enabled")
XR_DEFINE_BUILTIN(gc_count, "count", "(): float", "Get memory usage in KB")
XR_DEFINE_BUILTIN(gc_countb, "countb", "(): int", "Get memory usage in bytes")
XR_DEFINE_BUILTIN(gc_objects, "objects", "(): int", "Get total GC object count")
XR_DEFINE_BUILTIN(gc_gccount, "gccount", "(): int", "Get completed GC cycle count")
XR_DEFINE_BUILTIN(gc_debt, "debt", "(): int", "Get current GC debt in bytes")
XR_DEFINE_BUILTIN(gc_state, "state", "(): string", "Get current GC state name")
XR_DEFINE_BUILTIN(gc_info, "info", "(): Map", "Get detailed GC info as Map")
XR_DEFINE_BUILTIN(gc_timems, "timems", "(): float", "Get cumulative GC time in milliseconds")
XR_DEFINE_BUILTIN(gc_fragmentation, "fragmentation", "(): float", "Get heap fragmentation ratio (0.0-1.0)")
XR_DEFINE_BUILTIN(gc_setpause, "setpause", "(pause: int): int", "Set GC pause factor, return old value")
XR_DEFINE_BUILTIN(gc_setstepmul, "setstepmul", "(mul: int): int", "Set GC step multiplier, return old value")

XrModule* xr_load_module_gc(XrayIsolate *isolate) {
    XrModule *module = xr_module_create_native(isolate, "gc");
    
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, module, name_str, fn_val); \
        } while(0)
    
    // Control
    EXPORT_CFUNC("collect", gc_collect);
    EXPORT_CFUNC("step", gc_step);
    EXPORT_CFUNC("disable", gc_disable);
    EXPORT_CFUNC("enable", gc_enable);
    EXPORT_CFUNC("isrunning", gc_isrunning);
    
    // Statistics
    EXPORT_CFUNC("count", gc_count);
    EXPORT_CFUNC("countb", gc_countb);
    EXPORT_CFUNC("objects", gc_objects);
    EXPORT_CFUNC("gccount", gc_gccount);
    EXPORT_CFUNC("debt", gc_debt);
    EXPORT_CFUNC("state", gc_state);
    EXPORT_CFUNC("info", gc_info);
    EXPORT_CFUNC("timems", gc_timems);
    EXPORT_CFUNC("fragmentation", gc_fragmentation);
    
    // Tuning
    EXPORT_CFUNC("setpause", gc_setpause);
    EXPORT_CFUNC("setstepmul", gc_setstepmul);
    
    #undef EXPORT_CFUNC
    
    return module;
}
