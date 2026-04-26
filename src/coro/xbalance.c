/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbalance.c - Per-priority load balancing implementation
 *
 * KEY CONCEPT:
 *   Reduction-based load balance checking with per-priority awareness.
 *   Each priority level has independent migration paths.
 *   Periodically collects queue lengths and reductions across workers,
 *   then computes per-priority migration paths for overloaded workers.
 */

#include "xbalance.h"
#include "../base/xchecks.h"
#include "xworker.h"
#include "xcoroutine.h"
#include <stdatomic.h>
#include <string.h>
#include <limits.h>

// ========== Helper Functions ==========

// Map coroutine priority to runq index: LOW/NORMAL -> 0, HIGH -> 1
static inline int prio_to_runq(int prio) {
    return (prio == CORO_PRIORITY_HIGH) ? 1 : 0;
}

// ========== API Implementation ==========

struct XrWorker *xr_choose_target_worker(struct XrRuntime *runtime, int exclude) {
    if (!runtime || runtime->worker_count <= 1)
        return NULL;
    int n = runtime->worker_count;

    // Power-of-two-choices: sample 2 random workers, pick the one with lower load.
    // O(1) instead of O(N), statistically near-optimal load distribution.
    static _Atomic uint32_t rng = 1;
    uint32_t r = atomic_fetch_add_explicit(&rng, 1, memory_order_relaxed);
    // xorshift32 for cheap pseudo-random
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

    // Initialize migration paths: all targets = -1 (no migration)
    for (int i = 0; i < runtime->worker_count && i < XR_MAX_WORKERS; i++) {
        XrMigrationPath *mp = &runtime->migration_paths[i];
        mp->flags = 0;
        for (int p = 0; p < 3; p++) {
            mp->prio[p].limit_here = XR_MIGRATION_LIMIT_DEFAULT;
            mp->prio[p].limit_other = 0;
            mp->prio[p].target_worker = -1;
        }
    }
}

// Per-priority check_balance
// Collects per-priority queue lengths and reductions from each Worker,
// computes per-priority migration paths independently.
void xr_check_balance(struct XrRuntime *runtime, struct XrWorker *worker) {
    if (!runtime || !worker)
        return;

    // CAS to acquire checking lock
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

    // Per-priority statistics: queue length and reductions per worker
    int len[XR_MAX_WORKERS][XR_RUNQ_COUNT];
    int reds[XR_MAX_WORKERS][XR_RUNQ_COUNT];
    int total_reds[XR_RUNQ_COUNT] = {0};
    int total_len[XR_RUNQ_COUNT] = {0};

    for (int i = 0; i < wc; i++) {
        struct XrWorker *w = &runtime->workers[i];
        for (int p = 0; p < XR_RUNQ_COUNT; p++) {
            len[i][p] = xr_runq_len(&w->p.runq[p]);
            reds[i][p] = w->p.runq_reds[p];
            total_len[p] += len[i][p];
            total_reds[p] += reds[i][p];
        }
        // Track max_len for statistics
        for (int p = 0; p < XR_RUNQ_COUNT; p++) {
            if (len[i][p] > w->p.runq_max_len[p])
                w->p.runq_max_len[p] = len[i][p];
        }
    }

    // For each priority, compute migration paths independently
    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        int avg = total_len[p] / wc;
        int limit = avg > 0 ? avg : 1;

        // Find overloaded and underloaded workers
        int max_w = -1, min_w = -1;
        int max_l = 0, min_l = INT_MAX;

        for (int i = 0; i < wc; i++) {
            if (len[i][p] > max_l) {
                max_l = len[i][p];
                max_w = i;
            }
            if (len[i][p] < min_l) {
                min_l = len[i][p];
                min_w = i;
            }
        }

        // Set migration paths for this priority
        for (int i = 0; i < wc && i < XR_MAX_WORKERS; i++) {
            XrMigrationPath *mp = &runtime->migration_paths[i];
            mp->prio[p].limit_here = limit * 2;  // Emigrate if > 2x average
            mp->prio[p].limit_other = 0;

            if (len[i][p] > limit * 2 && min_w >= 0 && min_w != i) {
                // Overloaded: emigrate to lightest worker
                mp->prio[p].target_worker = min_w;
            } else if (len[i][p] == 0 && max_w >= 0 && max_w != i && max_l > limit) {
                // Empty: immigrate from heaviest worker
                mp->prio[p].target_worker = max_w;
            } else {
                mp->prio[p].target_worker = -1;
            }
        }
    }

    // Reset per-worker reduction counters and balance check counter
    for (int i = 0; i < wc; i++) {
        for (int p = 0; p < XR_RUNQ_COUNT; p++) {
            runtime->workers[i].p.runq_reds[p] = 0;
            runtime->workers[i].p.runq_max_len[p] = 0;
        }
    }
    worker->p.check_balance_reds = XR_CALL_CHECK_BALANCE_REDS;

    atomic_store(&runtime->balance_info.checking_balance, 0);
}

int xr_try_emigrate(struct XrWorker *worker) {
    if (!worker || !worker->p.runtime)
        return 0;
    struct XrRuntime *runtime = worker->p.runtime;
    int migrated = 0;

    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
        XrMigrationPath *mp = &runtime->migration_paths[worker->p.id];
        int target_id = mp->prio[p].target_worker;
        if (target_id < 0 || target_id >= runtime->worker_count)
            continue;

        int len = xr_runq_len(&worker->p.runq[p]);
        if (len <= mp->prio[p].limit_here)
            continue;

        struct XrWorker *target = &runtime->workers[target_id];
        int stolen = xr_runq_steal(&worker->p.runq[p], &target->p.runq[p], XR_MIGRATION_MAX_STEAL);
        if (stolen > 0) {
            mp->prio[p].target_worker = -1;  // Clear after migration
            migrated += stolen;
        }
    }
    return migrated;
}

// Record per-priority reductions and decrement balance check counter
void xr_worker_reductions_executed(struct XrWorker *worker, int prio, int reds) {
    if (!worker)
        return;
    int idx = prio_to_runq(prio);
    if (idx >= 0 && idx < XR_RUNQ_COUNT) {
        worker->p.runq_reds[idx] += reds;
    }
    worker->p.check_balance_reds -= reds;
}

void xr_runq_get_info(struct XrWorker *worker, int prio, int *len, int *reds) {
    if (!worker) {
        if (len)
            *len = 0;
        if (reds)
            *reds = 0;
        return;
    }
    int idx = prio_to_runq(prio);
    if (len)
        *len = xr_runq_len(&worker->p.runq[idx]);
    if (reds)
        *reds = (idx >= 0 && idx < XR_RUNQ_COUNT) ? worker->p.runq_reds[idx] : 0;
}
