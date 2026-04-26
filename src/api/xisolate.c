/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xisolate.c - Core Isolate lifecycle (new/delete)
 *
 * KEY CONCEPT:
 *   xray_isolate_new() creates a minimal runtime (VM + GC + string pool).
 *   Heavy subsystems are initialized via an optional callback (init_extra).
 *   This ensures the linker only pulls in heavy code when init_extra is set.
 *
 * WHY THIS DESIGN:
 *   The linker resolves symbols at the .o level. If xray_isolate.c directly
 *   calls xr_core_init / xr_module_system_init etc., those .o files get
 *   linked even if the call is behind an if-branch. Using a function pointer
 *   (init_extra) keeps this file free of heavy dependencies.
 *
 * RELATED MODULES:
 *   - xray_isolate_full.c: sets init_extra to pull in compiler/classes/etc
 *   - xray_isolate_tls.c: g_current_isolate + enter/exit
 *   - xray_isolate_params.c: params_init
 *   - xray_isolate_scripting.c: dostring/dofile (compiler-dependent)
 */

#include "../base/xlog.h"
#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xstring.h"
#include "../base/xmalloc.h"
#include "../runtime/xglobals_table.h"
#include "../coro/xcoroutine.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/object/xshape.h"
#include "../vm/xvm_profiler.h"
#include "../vm/xvm_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Isolate Creation ========== */

XrayIsolate* xray_isolate_new(const XrayIsolateParams *params) {
    XrayIsolate *isolate = (XrayIsolate *)xr_malloc(sizeof(XrayIsolate));
    if (!isolate) {
        xr_log_warning("isolate", "failed to allocate isolate");
        return NULL;
    }
    memset(isolate, 0, sizeof(XrayIsolate));
    isolate->ext_type_next = XR_TTASK + 1;

    if (params) {
        isolate->params = *params;
    } else {
        // Minimal init — no full-runtime callbacks unless caller sets them
        xray_isolate_params_init(&isolate->params);
    }
    isolate->userdata = isolate->params.userdata;
    isolate->init_flags = isolate->params.init_flags;

    // --- Core: string pool ---
    isolate->global_string_pool = xr_malloc(sizeof(XrGlobalStringPool));
    if (!isolate->global_string_pool) goto fail;
    memset(isolate->global_string_pool, 0, sizeof(XrGlobalStringPool));
    xr_global_pool_init(isolate->global_string_pool);

    // --- Core: GC ---
    xr_gc_init(&isolate->gc, isolate);

    // --- Core: system heap + main coroutine ---
    isolate->sys_heap = xr_malloc(sizeof(XrSystemHeap));
    if (!isolate->sys_heap) goto fail;
    if (!xr_sysheap_init(isolate->sys_heap, NULL)) goto fail;

    isolate->main_coro = xr_coro_create_bootstrap(isolate);
    if (!isolate->main_coro) goto fail;

    // --- Core: globals table ---
    isolate->globals = xr_globals_create(64);
    if (!isolate->globals) goto fail;

    // --- Core: VM engine ---
    if (xr_vm_init(isolate) != 0) goto fail;

#if XR_ENABLE_VM_PROFILER
    /* Allocate the per-isolate profiler eagerly when the build opted
     * in. The struct is small (~5KB) and pre-allocating avoids a
     * branch on every VM dispatch entry. NULL slot in disabled builds
     * keeps the field zero-cost. */
    isolate->profiler = xr_calloc(1, sizeof(VMProfiler));
    if (!isolate->profiler) goto fail_after_vm;
#endif

    // --- Core: shape registry (hidden classes) ---
    xr_shape_registry_init(isolate);

    // --- Optional: heavy subsystems via callback ---
    // init_extra is set by xray_isolate_full.c constructor (auto-registered).
    // For XR_INIT_RUNTIME mode, init_extra stays NULL → no heavy deps linked.
    if (isolate->params.init_extra) {
        if (isolate->params.init_extra(isolate) != 0) {
            goto fail_after_vm;
        }
    }

    xray_isolate_enter(isolate);
    return isolate;

fail_after_vm:
    xr_vm_cleanup(isolate);
fail:
    if (isolate->globals) xr_globals_destroy((XrGlobalsTable*)isolate->globals);
    if (isolate->sys_heap) { xr_sysheap_destroy(isolate->sys_heap); xr_free(isolate->sys_heap); }
    xr_gc_cleanup(&isolate->gc);
    if (isolate->global_string_pool) { xr_global_pool_free(isolate->global_string_pool); xr_free(isolate->global_string_pool); }
    xr_free(isolate);
    return NULL;
}

/* ========== Isolate Deletion ========== */

void xray_isolate_delete(XrayIsolate *isolate) {
    if (!isolate) return;

    /* Drain the per-isolate profiler before any structure that
     * powers the report (opcode info, isolate pointer) goes away.
     * vm_profiler_report tolerates NULL so this is a no-op when
     * the build never compiled the profiler in. */
    vm_profiler_report((const VMProfiler *)isolate->profiler);
    if (isolate->profiler) {
        xr_free(isolate->profiler);
        isolate->profiler = NULL;
    }

    if (g_current_isolate == isolate) {
        xray_isolate_exit();
    }

    if (isolate->main_coro) {
        xr_coro_free(isolate->main_coro);
        isolate->main_coro = NULL;
    }

    // Cleanup via callback (mirrors init_extra)
    if (isolate->params.cleanup_extra) {
        isolate->params.cleanup_extra(isolate);
    }

    xr_vm_cleanup(isolate);

    xr_shape_registry_destroy(isolate);

    // The globals table stores XrValue entries that reference fixedgc
    // bodies (enum types and the like). Drop the table BEFORE
    // xr_gc_cleanup so any post-VM hook that scans globals during
    // teardown still sees consistent pointers, and so xr_gc_cleanup is
    // the single authoritative free path for those bodies.
    if (isolate->globals) {
        xr_globals_destroy((XrGlobalsTable*)isolate->globals);
        isolate->globals = NULL;
    }

    xr_gc_cleanup(&isolate->gc);

    if (isolate->sys_heap) {
        xr_sysheap_destroy(isolate->sys_heap);
        xr_free(isolate->sys_heap);
        isolate->sys_heap = NULL;
    }

    if (isolate->global_string_pool) {
        xr_global_pool_free(isolate->global_string_pool);
        xr_free(isolate->global_string_pool);
        isolate->global_string_pool = NULL;
    }

    // Release the lazy stdlib per-isolate cache. The struct only holds
    // pointers to GC-managed objects (shapes, interned XrValues), so
    // freeing the container itself is sufficient; defined as an opaque
    // void* in xisolate_internal.h to avoid leaking stdlib types into
    // the core header (see stdlib/stdlib_cache.h).
    if (isolate->stdlib_cache) {
        xr_free(isolate->stdlib_cache);
        isolate->stdlib_cache = NULL;
    }

    if (isolate->config) {
        xr_free(isolate->config);
        isolate->config = NULL;
    }

    xr_free(isolate);
}

// For bytecode serialization
void* xr_isolate_get_symbol_table(XrayIsolate *isolate) {
    if (!isolate) return NULL;
    return isolate->symbol_table;
}

/* ========== Advanced API ========== */

XrayBackendType xray_isolate_get_backend(XrayIsolate *isolate) {
    xray_api_checkr(isolate != NULL, "xray_isolate_get_backend: NULL isolate", 0);
    return isolate->params.backend_type;
}

void xray_isolate_set_userdata(XrayIsolate *isolate, void *userdata) {
    xray_api_check(isolate != NULL, "xray_isolate_set_userdata: NULL isolate");
    isolate->userdata = userdata;
}

void* xray_isolate_get_userdata(XrayIsolate *isolate) {
    xray_api_checkr(isolate != NULL, "xray_isolate_get_userdata: NULL isolate", NULL);
    return isolate->userdata;
}

/* ========== Statistics and Debugging ========== */

void xray_isolate_get_stats(XrayIsolate *isolate,
                            size_t *bytes_allocated,
                            int *gc_count) {
    xray_api_check(isolate != NULL, "xray_isolate_get_stats: NULL isolate");
    if (bytes_allocated) *bytes_allocated = (size_t)isolate->gc.totalbytes;
    if (gc_count) {
        XrCoroGC *coro_gc = xr_isolate_get_coro_gc(isolate);
        *gc_count = coro_gc ? (int)coro_gc->gc_count : 0;
    }
}

void xray_isolate_collect_garbage(XrayIsolate *isolate) {
    xray_api_check(isolate != NULL, "xray_isolate_collect_garbage: NULL isolate");
    XrCoroGC *coro_gc = xr_isolate_get_coro_gc(isolate);
    if (coro_gc) xr_coro_gc_fullgc(coro_gc);
}

void xray_isolate_set_trace(XrayIsolate *isolate, bool enable) {
    xray_api_check(isolate != NULL, "xray_isolate_set_trace: NULL isolate");
    isolate->params.trace_execution = enable;
    isolate->vm.trace_execution = enable;
}

void xray_isolate_set_dump_bytecode(XrayIsolate *isolate, bool enable) {
    xray_api_check(isolate != NULL, "xray_isolate_set_dump_bytecode: NULL isolate");
    isolate->params.dump_bytecode = enable;
}
