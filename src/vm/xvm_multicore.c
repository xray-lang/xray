/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_multicore.c - VM isolate wrappers for the scheduler runtime
 */

#include "xvm_internal.h"
#include "../coro/xthread_obj.h"
#include "../coro/xworker.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xisolate_internal.h"
#include "../os/os_time.h"
#include <stdio.h>

void xray_vm_multicore_init(XrVMRuntime *X, int num_workers) {
    if (!X)
        return;

    XrRuntime *runtime = xr_scheduler_runtime_new(xr_isolate_get_runtime_core(X), num_workers);
    if (!runtime)
        return;

    xr_scheduler_runtime_attach_isolate(runtime, X);
    X->vm.scheduler = runtime;

    if (X->main_coro) {
        X->main_coro->core = xr_isolate_get_runtime_core(X);
        X->main_coro->scheduler = runtime;
    }

    xr_runtime_start(runtime);
}

void xray_vm_multicore_destroy(XrVMRuntime *X) {
    if (!X || !X->vm.scheduler)
        return;

    xr_thread_obj_drain_isolate(X);

    XrRuntime *runtime = (XrRuntime *) X->vm.scheduler;
    bool stats_enabled = xr_sched_stats_enabled(runtime);
    uint64_t total_start_ns = xr_time_monotonic_ns();
    uint64_t stage_start_ns = total_start_ns;

    if (X->main_coro && X->main_coro->scheduler == runtime)
        X->main_coro->scheduler = NULL;

    xr_runtime_stop(runtime);
    uint64_t stop_ms = (xr_time_monotonic_ns() - stage_start_ns) / 1000000ULL;

    stage_start_ns = xr_time_monotonic_ns();
    xr_scheduler_runtime_delete(runtime);
    uint64_t destroy_ms = (xr_time_monotonic_ns() - stage_start_ns) / 1000000ULL;

    if (stats_enabled) {
        uint64_t total_ms = (xr_time_monotonic_ns() - total_start_ns) / 1000000ULL;
        fprintf(stderr, "Multicore teardown: stop_ms=%llu destroy_ms=%llu total_ms=%llu\n",
                (unsigned long long) stop_ms, (unsigned long long) destroy_ms,
                (unsigned long long) total_ms);
    }

    X->vm.scheduler = NULL;
}
