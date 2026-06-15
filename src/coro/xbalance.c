/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbalance.c - Load balancing implementation
 *
 * KEY CONCEPT:
 *   Reduction-based load balance checking. The balancer periodically
 *   collects each worker's local ready-queue length and reduction count,
 *   then records a migration path from overloaded workers to light workers.
 */

#include "xbalance.h"
#include "../base/xchecks.h"
#include "xcoro_tuning.h"
#include "xworker.h"
#include "xcoroutine.h"
#include <stdatomic.h>
#include <string.h>
#include <limits.h>

// ========== API Implementation ==========

struct XrWorker *xr_choose_target_worker(struct XrRuntime *runtime, int exclude) {
    if (!runtime || runtime->worker_count <= 1)
        return NULL;
    int n = runtime->worker_count;

    // Power-of-two-choices: sample 2 random workers, pick the lower load.
    static _Atomic uint32_t rng = 1;
    uint32_t r = atomic_fetch_add_explicit(&rng, 1, memory_order_relaxed);
    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;

    int a = (int) (r % n);
    int b = (int) ((r >> 16) % n);
    if (a == exclude)
        a = (a + 1) % n;
    if (b == exclude || b == a)
        b = (b + 1) % n;
    if (b == exclude)
        b = (b + 1) % n;
    if (b >= n)
        b = 0;

    int la = xr_worker_total_queue_len(&runtime->workers[a]);
    int lb = xr_worker_total_queue_len(&runtime->workers[b]);
    return (la <= lb) ? &runtime->workers[a] : &runtime->workers[b];
}

void xr_balance_init(struct XrRuntime *runtime) {
    if (!runtime)
        return;

    atomic_store(&runtime->balance_info.checking_balance, 0);
    runtime->balance_info.last_active_workers = runtime->worker_count;
    runtime->balance_info.halftime = 1;
    runtime->balance_info.full_reds_history_index = 0;

    for (int i = 0; i < runtime->worker_count; i++) {
        runtime->workers[i].p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;
    }

    for (int i = 0; i < runtime->worker_count && i < XR_MAX_WORKERS; i++) {
        XrMigrationPath *mp = &runtime->migration_paths[i];
        mp->flags = 0;
        atomic_store_explicit(&mp->runq.limit_here, XR_MIGRATION_LIMIT_DEFAULT,
                              memory_order_relaxed);
        atomic_store_explicit(&mp->runq.limit_other, 0, memory_order_relaxed);
        atomic_store_explicit(&mp->runq.target_worker, -1, memory_order_relaxed);
    }
}

void xr_check_balance(struct XrRuntime *runtime, struct XrWorker *worker) {
    if (!runtime || !worker)
        return;

    int expected = 0;
    if (!atomic_compare_exchange_strong(&runtime->balance_info.checking_balance, &expected, 1)) {
        worker->p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;
        return;
    }

    int wc = runtime->worker_count;
    if (wc <= 1) {
        worker->p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;
        atomic_store(&runtime->balance_info.checking_balance, 0);
        return;
    }

    int len[XR_MAX_WORKERS];
    int total_len = 0;
    int max_w = -1;
    int min_w = -1;
    int max_l = 0;
    int min_l = INT_MAX;

    for (int i = 0; i < wc; i++) {
        struct XrWorker *w = &runtime->workers[i];
        len[i] = xr_runq_len(&w->p.runq);
        total_len += len[i];
        int prev_max = atomic_load_explicit(&w->p.runq_max_len, memory_order_relaxed);
        while (len[i] > prev_max &&
               !atomic_compare_exchange_weak_explicit(&w->p.runq_max_len, &prev_max, len[i],
                                                      memory_order_relaxed, memory_order_relaxed)) {
        }
        if (len[i] > max_l) {
            max_l = len[i];
            max_w = i;
        }
        if (len[i] < min_l) {
            min_l = len[i];
            min_w = i;
        }
    }

    int avg = total_len / wc;
    int limit = avg > 0 ? avg : 1;
    for (int i = 0; i < wc && i < XR_MAX_WORKERS; i++) {
        XrMigrationPath *mp = &runtime->migration_paths[i];
        int limit_here = limit * XR_MIGRATION_THRESHOLD_MULTIPLIER;
        atomic_store_explicit(&mp->runq.limit_here, limit_here, memory_order_relaxed);
        atomic_store_explicit(&mp->runq.limit_other, 0, memory_order_relaxed);

        if (len[i] > limit_here && min_w >= 0 && min_w != i) {
            atomic_store_explicit(&mp->runq.target_worker, min_w, memory_order_relaxed);
        } else if (len[i] == 0 && max_w >= 0 && max_w != i && max_l > limit) {
            atomic_store_explicit(&mp->runq.target_worker, max_w, memory_order_relaxed);
        } else {
            atomic_store_explicit(&mp->runq.target_worker, -1, memory_order_relaxed);
        }
    }

    for (int i = 0; i < wc; i++) {
        atomic_store_explicit(&runtime->workers[i].p.runq_reds, 0, memory_order_relaxed);
        atomic_store_explicit(&runtime->workers[i].p.runq_max_len, 0, memory_order_relaxed);
    }
    worker->p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;

    atomic_store(&runtime->balance_info.checking_balance, 0);
}

int xr_try_emigrate(struct XrWorker *worker) {
    if (!worker || !worker->p.runtime)
        return 0;
    struct XrRuntime *runtime = worker->p.runtime;
    XrMigrationPath *mp = &runtime->migration_paths[worker->p.id];
    int target_id = atomic_load_explicit(&mp->runq.target_worker, memory_order_relaxed);
    if (target_id < 0 || target_id >= runtime->worker_count)
        return 0;

    int len = xr_runq_len(&worker->p.runq);
    int limit_here = atomic_load_explicit(&mp->runq.limit_here, memory_order_relaxed);
    if (len <= limit_here)
        return 0;

    struct XrWorker *target = &runtime->workers[target_id];
    int stolen = xr_runq_steal(&worker->p.runq, &target->p.runq, XR_MIGRATION_MAX_STEAL);
    if (stolen > 0) {
        atomic_store_explicit(&mp->runq.target_worker, -1, memory_order_relaxed);
        xr_proc_local_runq_inc(&target->p, stolen);
    }
    return stolen;
}

void xr_worker_reductions_executed(struct XrWorker *worker, int reds) {
    if (!worker)
        return;
    atomic_fetch_add_explicit(&worker->p.runq_reds, reds, memory_order_relaxed);
    worker->p.check_balance_reds -= reds;
}

void xr_runq_get_info(struct XrWorker *worker, int *len, int *reds) {
    if (!worker) {
        if (len)
            *len = 0;
        if (reds)
            *reds = 0;
        return;
    }
    if (len)
        *len = xr_runq_len(&worker->p.runq);
    if (reds)
        *reds = atomic_load_explicit(&worker->p.runq_reds, memory_order_relaxed);
}
