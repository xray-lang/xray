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
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../base/xmalloc.h"
#include "../runtime/xglobals_table.h"
#include "../coro/xcoroutine.h"
#include "../coro/xtask.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm_profiler.h"
#include "../vm/xvm_internal.h"
#include "../../stdlib/stdlib_cache.h"
#include "../os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool isolate_teardown_stats_enabled(void) {
    const char *value = getenv("XRAY_SCHED_STATS");
    if (!value || value[0] == '\0' || value[0] == '0')
        return false;
    if (value[0] == 'f' || value[0] == 'F')
        return false;
    if (value[0] == 'n' || value[0] == 'N')
        return false;
    return true;
}

static uint64_t isolate_teardown_elapsed_ms(uint64_t start_ns) {
    uint64_t now_ns = xr_time_monotonic_ns();
    return (now_ns >= start_ns) ? (now_ns - start_ns) / 1000000ULL : 0;
}

/* ========== Isolate Creation ========== */

XrayIsolate *xray_isolate_new(const XrayIsolateParams *params) {
    XrayIsolate *isolate = (XrayIsolate *) xr_malloc(sizeof(XrayIsolate));
    if (!isolate) {
        xr_log_warning("isolate", "failed to allocate isolate");
        return NULL;
    }
    memset(isolate, 0, sizeof(XrayIsolate));

    if (params) {
        isolate->params = *params;
    } else {
        // Minimal init — no full-runtime callbacks unless caller sets them
        xray_isolate_params_init(&isolate->params);
    }
    isolate->init_flags = isolate->params.init_flags;

    XrRuntimeCoreConfig core_cfg = {
        .owner_isolate = isolate,
        .userdata = isolate->params.userdata,
    };
    isolate->core_rt = xr_runtime_core_new(&core_cfg);
    if (!isolate->core_rt)
        goto fail;
    xr_script_info_set(&isolate->core_rt->script_info, isolate->params.script_file,
                       isolate->params.script_argc, isolate->params.script_argv);

    // --- VM main coroutine ---
    isolate->main_coro = xr_coro_create_bootstrap(isolate);
    if (!isolate->main_coro)
        goto fail;

    // --- Core: globals table ---
    isolate->globals = xr_globals_create(64);
    if (!isolate->globals)
        goto fail;

    // --- Core: VM engine ---
    if (xr_vm_init(isolate) != 0)
        goto fail;

#if XR_ENABLE_VM_PROFILER
    /* Allocate the per-isolate profiler eagerly when the build opted
     * in. The struct is small (~5KB) and pre-allocating avoids a
     * branch on every VM dispatch entry. NULL slot in disabled builds
     * keeps the field zero-cost. */
    isolate->profiler = xr_calloc(1, sizeof(VMProfiler));
    if (!isolate->profiler)
        goto fail_after_vm;
#endif

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
    if (isolate->globals)
        xr_globals_destroy((XrGlobalsTable *) isolate->globals);
    xr_runtime_core_delete(isolate->core_rt);
    xr_free(isolate);
    return NULL;
}

/* ========== Isolate Deletion ========== */

void xray_isolate_delete(XrayIsolate *isolate) {
    if (!isolate)
        return;

    bool teardown_stats = isolate_teardown_stats_enabled();
    uint64_t teardown_start_ns = xr_time_monotonic_ns();
    uint64_t stage_start_ns = teardown_start_ns;
    uint64_t profiler_ms = 0;
    uint64_t runtime_ms = 0;
    uint64_t tls_exit_ms = 0;
    uint64_t main_coro_ms = 0;
    uint64_t cleanup_extra_ms = 0;
    uint64_t vm_cleanup_ms = 0;
    uint64_t tmp_strbuf_ms = 0;
    uint64_t globals_ms = 0;
    uint64_t coro_storage_ms = 0;
    uint64_t gc_cleanup_ms = 0;
    uint64_t deferred_tasks_ms = 0;
    uint64_t sys_heap_ms = 0;
    uint64_t string_pool_ms = 0;
    uint64_t stdlib_cache_ms = 0;
    uint64_t config_ms = 0;

    /* Drain the per-isolate profiler before any structure that
     * powers the report (opcode info, isolate pointer) goes away.
     * vm_profiler_report tolerates NULL so this is a no-op when
     * the build never compiled the profiler in. */
    stage_start_ns = xr_time_monotonic_ns();
    vm_profiler_report((const VMProfiler *) isolate->profiler);
    if (isolate->profiler) {
        xr_free(isolate->profiler);
        isolate->profiler = NULL;
    }
    profiler_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    /* Runtime-owned Task shells are still referenced by coroutine heaps
     * (for example Array<Task> locals in main). Destroy the runtime first
     * so workers and pools are drained, but defer the Task shell free until
     * after all isolate GC roots have released their XrValue references. */
    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->scheduler_runtime) {
        xr_multicore_destroy(isolate);
    }
    runtime_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (g_current_isolate == isolate) {
        xray_isolate_exit();
    }
    tls_exit_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->main_coro) {
        xr_coro_free(isolate->main_coro);
        isolate->main_coro = NULL;
    }
    main_coro_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    // Cleanup via callback (mirrors init_extra)
    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->params.cleanup_extra) {
        isolate->params.cleanup_extra(isolate);
    }
    cleanup_extra_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_vm_cleanup(isolate);
    vm_cleanup_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_runtime_core_free_tmp_strbuf(isolate->core_rt);
    tmp_strbuf_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    // The globals table stores XrValue entries that reference fixedgc
    // bodies (enum types and the like). Drop the table BEFORE
    // xr_gc_cleanup so any post-VM hook that scans globals during
    // teardown still sees consistent pointers, and so xr_gc_cleanup is
    // the single authoritative free path for those bodies.
    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->globals) {
        xr_globals_destroy((XrGlobalsTable *) isolate->globals);
        isolate->globals = NULL;
    }
    globals_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    /* Coroutine shells must release their per-coroutine heaps before
     * fixed GC finalization. Class/module metadata remains alive until
     * fixed GC destroy hooks finish reading instance layouts. */
    xr_runtime_core_destroy_coro_storage(isolate->core_rt);
    coro_storage_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_runtime_core_cleanup_gc(isolate->core_rt);
    gc_cleanup_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_task_isolate_destroy_deferred(isolate);
    deferred_tasks_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->core_rt && isolate->core_rt->sys_heap) {
        xr_sysheap_destroy(isolate->core_rt->sys_heap);
        xr_free(isolate->core_rt->sys_heap);
        isolate->core_rt->sys_heap = NULL;
    }
    sys_heap_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->core_rt && isolate->core_rt->global_string_pool) {
        xr_global_pool_free(isolate->core_rt->global_string_pool);
        xr_free(isolate->core_rt->global_string_pool);
        isolate->core_rt->global_string_pool = NULL;
    }
    string_pool_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    // Release the lazy stdlib per-isolate cache. Calls log_state_cleanup
    // and frees the container. Safe to call even when cache is NULL.
    stage_start_ns = xr_time_monotonic_ns();
    xr_stdlib_cache_free(isolate);
    stdlib_cache_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->core_rt && isolate->core_rt->config) {
        xr_free(isolate->core_rt->config);
        isolate->core_rt->config = NULL;
    }
    config_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    xr_runtime_core_delete(isolate->core_rt);
    isolate->core_rt = NULL;

    if (teardown_stats) {
        fprintf(stderr,
                "Isolate teardown: profiler_ms=%llu runtime_ms=%llu tls_exit_ms=%llu "
                "main_coro_ms=%llu cleanup_extra_ms=%llu vm_cleanup_ms=%llu "
                "tmp_strbuf_ms=%llu globals_ms=%llu coro_storage_ms=%llu "
                "gc_cleanup_ms=%llu deferred_tasks_ms=%llu sys_heap_ms=%llu "
                "string_pool_ms=%llu stdlib_cache_ms=%llu config_ms=%llu total_ms=%llu\n",
                (unsigned long long) profiler_ms, (unsigned long long) runtime_ms,
                (unsigned long long) tls_exit_ms, (unsigned long long) main_coro_ms,
                (unsigned long long) cleanup_extra_ms, (unsigned long long) vm_cleanup_ms,
                (unsigned long long) tmp_strbuf_ms, (unsigned long long) globals_ms,
                (unsigned long long) coro_storage_ms, (unsigned long long) gc_cleanup_ms,
                (unsigned long long) deferred_tasks_ms, (unsigned long long) sys_heap_ms,
                (unsigned long long) string_pool_ms, (unsigned long long) stdlib_cache_ms,
                (unsigned long long) config_ms,
                (unsigned long long) isolate_teardown_elapsed_ms(teardown_start_ns));
    }

    xr_free(isolate);
}

/* ========== Advanced API ========== */

XrayBackendType xray_isolate_get_backend(XrayIsolate *isolate) {
    xray_api_checkr(isolate != NULL, "xray_isolate_get_backend: NULL isolate", 0);
    return isolate->params.backend_type;
}

void xray_isolate_set_userdata(XrayIsolate *isolate, void *userdata) {
    xray_api_check(isolate != NULL, "xray_isolate_set_userdata: NULL isolate");
    if (isolate->core_rt)
        isolate->core_rt->userdata = userdata;
}

void *xray_isolate_get_userdata(XrayIsolate *isolate) {
    xray_api_checkr(isolate != NULL, "xray_isolate_get_userdata: NULL isolate", NULL);
    return isolate->core_rt ? isolate->core_rt->userdata : NULL;
}

/* ========== Statistics and Debugging ========== */

void xray_isolate_get_stats(XrayIsolate *isolate, size_t *bytes_allocated, int *gc_count) {
    xray_api_check(isolate != NULL, "xray_isolate_get_stats: NULL isolate");
    if (bytes_allocated)
        *bytes_allocated = isolate->core_rt ? (size_t) isolate->core_rt->gc.totalbytes : (size_t) 0;
    if (gc_count) {
        XrCoroGC *coro_gc = xr_isolate_get_coro_gc(isolate);
        *gc_count = coro_gc ? (int) coro_gc->gc_count : 0;
    }
}

void xray_isolate_collect_garbage(XrayIsolate *isolate) {
    xray_api_check(isolate != NULL, "xray_isolate_collect_garbage: NULL isolate");
    XrCoroGC *coro_gc = xr_isolate_get_coro_gc(isolate);
    if (coro_gc)
        xr_coro_gc_fullgc(coro_gc);
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
