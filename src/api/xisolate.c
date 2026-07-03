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
 *   xray_vm_new() creates a minimal bytecode VM runtime.
 *   Heavy variants use explicit constructors in separate translation units so
 *   bytecode-only embedders do not pull in compiler/frontend code.
 *
 * WHY THIS DESIGN:
 *   The linker resolves symbols at the .o level. If xray_isolate.c directly
 *   calls xr_core_init / xr_module_system_init etc., those .o files get
 *   linked even if the call is behind an if-branch. Keep heavy constructors
 *   in their own files and let them install a private cleanup hook.
 *
 * RELATED MODULES:
 *   - xisolate_full.c: explicit full VM constructor
 *   - xisolate_runtime.c: explicit runtime-ABI VM constructor
 *   - xisolate_tls.c: g_current_isolate + enter/exit
 *   - xisolate_params.c: config_init
 *   - xisolate_scripting.c: dostring/dofile (compiler-dependent)
 */

#include "../base/xlog.h"
#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xobj_destroy_ops.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../base/xmalloc.h"
#include "../runtime/xglobals_table.h"
#include "../coro/xcoroutine.h"
#include "../coro/xtask.h"
#include "../runtime/mem/xcoro_heap.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm_profiler.h"
#include "../vm/xvm_internal.h"
#include "../coro/xthread_obj.h"
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

XrVMRuntime *xray_vm_new(const XrVMConfig *params) {
    XrVMRuntime *isolate = (XrVMRuntime *) xr_malloc(sizeof(XrVMRuntime));
    if (!isolate) {
        xr_log_warning("isolate", "failed to allocate isolate");
        return NULL;
    }
    memset(isolate, 0, sizeof(XrVMRuntime));

    if (params) {
        isolate->params = *params;
    } else {
        // Minimal init — no full-runtime callbacks unless caller sets them
        xray_vm_config_init(&isolate->params);
    }
    XrRuntimeCoreConfig core_cfg = {
        .owner_isolate = isolate,
        .userdata = isolate->params.userdata,
    };
    isolate->core_rt = xr_runtime_core_new(&core_cfg);
    if (!isolate->core_rt)
        goto fail;
    xr_runtime_core_enable_basic_destroy_ops(isolate->core_rt);
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

    xray_vm_enter(isolate);
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

void xray_vm_delete(XrVMRuntime *isolate) {
    if (!isolate)
        return;

    bool teardown_stats = isolate_teardown_stats_enabled();
    uint64_t teardown_start_ns = xr_time_monotonic_ns();
    uint64_t stage_start_ns = teardown_start_ns;
    uint64_t profiler_ms = 0;
    uint64_t sys_thread_drain_ms = 0;
    uint64_t runtime_ms = 0;
    uint64_t tls_exit_ms = 0;
    uint64_t main_coro_ms = 0;
    uint64_t lifecycle_cleanup_ms = 0;
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

    /* Detached VM sys.Thread entries still execute against this isolate even
     * after user code has returned. Drain them before tearing down scheduler,
     * profiler, TLS, coroutine heaps, or the shared runtime core they may still
     * touch. */
    stage_start_ns = xr_time_monotonic_ns();
    xr_thread_obj_drain_isolate(isolate);
    sys_thread_drain_ms = isolate_teardown_elapsed_ms(stage_start_ns);

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
    if (isolate->vm.scheduler) {
        xray_vm_multicore_destroy(isolate);
    }
    runtime_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (g_current_isolate == isolate) {
        xray_vm_exit();
    }
    tls_exit_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->main_coro) {
        xr_coro_free(isolate->main_coro);
        isolate->main_coro = NULL;
    }
    main_coro_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    // Cleanup private lifecycle state installed by explicit heavy constructors.
    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->lifecycle_cleanup) {
        isolate->lifecycle_cleanup(isolate);
        isolate->lifecycle_cleanup = NULL;
    }
    lifecycle_cleanup_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_vm_cleanup(isolate);
    vm_cleanup_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_runtime_core_free_tmp_strbuf(isolate->core_rt);
    tmp_strbuf_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    // The globals table stores XrValue entries that reference fixed heap
    // bodies (enum types and the like). Drop the table BEFORE
    // xr_fixed_heap_cleanup so any post-VM hook that scans globals during
    // teardown still sees consistent pointers, and so xr_fixed_heap_cleanup is
    // the single authoritative free path for those bodies.
    stage_start_ns = xr_time_monotonic_ns();
    if (isolate->globals) {
        xr_globals_destroy((XrGlobalsTable *) isolate->globals);
        isolate->globals = NULL;
    }
    globals_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    /* Coroutine shells must release their per-coroutine heaps before
     * fixed heap finalization. Class/module metadata remains alive until
     * fixed object destroy hooks finish reading instance layouts. */
    xr_runtime_core_destroy_coro_storage(isolate->core_rt);
    coro_storage_ms = isolate_teardown_elapsed_ms(stage_start_ns);

    stage_start_ns = xr_time_monotonic_ns();
    xr_runtime_core_cleanup_fixed_heap(isolate->core_rt);
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
                "sys_thread_drain_ms=%llu main_coro_ms=%llu "
                "lifecycle_cleanup_ms=%llu vm_cleanup_ms=%llu "
                "tmp_strbuf_ms=%llu globals_ms=%llu coro_storage_ms=%llu "
                "gc_cleanup_ms=%llu deferred_tasks_ms=%llu sys_heap_ms=%llu "
                "string_pool_ms=%llu stdlib_cache_ms=%llu config_ms=%llu total_ms=%llu\n",
                (unsigned long long) profiler_ms, (unsigned long long) runtime_ms,
                (unsigned long long) tls_exit_ms, (unsigned long long) sys_thread_drain_ms,
                (unsigned long long) main_coro_ms, (unsigned long long) lifecycle_cleanup_ms,
                (unsigned long long) vm_cleanup_ms, (unsigned long long) tmp_strbuf_ms,
                (unsigned long long) globals_ms, (unsigned long long) coro_storage_ms,
                (unsigned long long) gc_cleanup_ms, (unsigned long long) deferred_tasks_ms,
                (unsigned long long) sys_heap_ms, (unsigned long long) string_pool_ms,
                (unsigned long long) stdlib_cache_ms, (unsigned long long) config_ms,
                (unsigned long long) isolate_teardown_elapsed_ms(teardown_start_ns));
    }

    xr_free(isolate);
}

/* ========== Advanced API ========== */

XrVMBackendType xray_vm_get_backend(XrVMRuntime *isolate) {
    xray_api_checkr(isolate != NULL, "xray_vm_get_backend: NULL isolate", 0);
    return isolate->params.backend_type;
}

void xray_vm_set_userdata(XrVMRuntime *isolate, void *userdata) {
    xray_api_check(isolate != NULL, "xray_vm_set_userdata: NULL isolate");
    if (isolate->core_rt)
        isolate->core_rt->userdata = userdata;
}

void *xray_vm_get_userdata(XrVMRuntime *isolate) {
    xray_api_checkr(isolate != NULL, "xray_vm_get_userdata: NULL isolate", NULL);
    return isolate->core_rt ? isolate->core_rt->userdata : NULL;
}

/* ========== Statistics and Debugging ========== */

void xray_vm_get_stats(XrVMRuntime *isolate, size_t *bytes_allocated, int *cycle_count) {
    xray_api_check(isolate != NULL, "xray_vm_get_stats: NULL isolate");
    if (bytes_allocated)
        *bytes_allocated =
            isolate->core_rt ? (size_t) isolate->core_rt->fixed_heap.totalbytes : (size_t) 0;
    if (cycle_count) {
        XrCoroHeap *heap = xr_isolate_get_heap(isolate);
        *cycle_count = heap ? (int) heap->cycle_collect_count : 0;
    }
}

void xray_vm_collect_cycles(XrVMRuntime *isolate) {
    xray_api_check(isolate != NULL, "xray_vm_collect_cycles: NULL isolate");
    XrCoroHeap *heap = xr_isolate_get_heap(isolate);
    if (heap)
        xr_coro_heap_collect_cycles(heap);
}

void xray_vm_set_trace(XrVMRuntime *isolate, bool enable) {
    xray_api_check(isolate != NULL, "xray_vm_set_trace: NULL isolate");
    isolate->params.trace_execution = enable;
    isolate->vm.trace_execution = enable;
}

void xray_vm_set_dump_bytecode(XrVMRuntime *isolate, bool enable) {
    xray_api_check(isolate != NULL, "xray_vm_set_dump_bytecode: NULL isolate");
    isolate->params.dump_bytecode = enable;
}
