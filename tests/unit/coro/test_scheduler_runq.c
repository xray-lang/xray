/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_scheduler_runq.c - Unit tests for the single scheduler run queue
 */

#include "../test_framework.h"
#include "base/xconstants.h"
#include "coro/xcoro_tuning.h"
#include "coro/xscheduler_policy.h"
#include "coro/xworker_internal.h"
#include "runtime/gc/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

typedef struct SchedulerFixture {
    XrayIsolate isolate_storage;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker worker;
    XrMachine machine;
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    bool sys_heap_initialized;
    bool worker_initialized;
    bool inject_initialized;
} SchedulerFixture;

static void fixture_init_runtime(XrRuntime *runtime, XrayIsolate *isolate, XrWorker *workers,
                                 XrMachine *machines, int worker_count) {
    memset(runtime, 0, sizeof(*runtime));
    runtime->isolate = isolate;
    runtime->worker_count = worker_count;
    runtime->workers = workers;
    runtime->machines = machines;
    atomic_store(&runtime->running, true);
    atomic_store(&runtime->threads_started, false);
    atomic_store(&runtime->spinning_count, 0);
    atomic_store(&runtime->idle_worker_count, 0);
    atomic_store(&runtime->total_inbox_len, 0);
    atomic_store(&runtime->nonempty_p_mask, 0);
    atomic_store(&runtime->stealable_p_mask, 0);
    atomic_store(&runtime->timer_p_mask, 0);
    atomic_store(&runtime->idle_p_mask, 0);
    atomic_store(&runtime->searching_count, 0);
}

static bool scheduler_fixture_init(SchedulerFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->isolate_storage.sys_heap = &f->sys_heap;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    fixture_init_runtime(&f->runtime, &f->isolate_storage, &f->worker, &f->machine, 1);
    xr_injectq_init(&f->runtime);
    f->inject_initialized = true;

    xr_worker_init(&f->worker, 0, &f->runtime);
    f->worker_initialized = true;

    f->isolate_storage.vm.runtime = &f->runtime;
    tls_current_worker = &f->worker;
    tls_current_machine = &f->machine;
    return true;
}

static void scheduler_fixture_cleanup(SchedulerFixture *f) {
    if (f->worker_initialized) {
        xr_worker_destroy(&f->worker);
        f->worker_initialized = false;
    }
    if (f->inject_initialized) {
        xr_injectq_destroy(&f->runtime);
        f->inject_initialized = false;
    }
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

typedef struct StealFixture {
    XrayIsolate isolate_storage;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker workers[2];
    XrMachine machines[2];
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    bool sys_heap_initialized;
    bool inject_initialized;
    bool worker_initialized[2];
} StealFixture;

static bool steal_fixture_init(StealFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->isolate_storage.sys_heap = &f->sys_heap;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    fixture_init_runtime(&f->runtime, &f->isolate_storage, f->workers, f->machines, 2);
    xr_injectq_init(&f->runtime);
    f->inject_initialized = true;

    for (int i = 0; i < 2; i++) {
        xr_worker_init(&f->workers[i], i, &f->runtime);
        f->worker_initialized[i] = true;
        f->workers[i].p.rng_state = (uint32_t) (0x12345678u + (uint32_t) i);
    }

    f->isolate_storage.vm.runtime = &f->runtime;
    tls_current_worker = NULL;
    tls_current_machine = NULL;
    return true;
}

static void steal_fixture_cleanup(StealFixture *f) {
    for (int i = 1; i >= 0; i--) {
        if (f->worker_initialized[i]) {
            xr_worker_destroy(&f->workers[i]);
            f->worker_initialized[i] = false;
        }
    }
    if (f->inject_initialized) {
        xr_injectq_destroy(&f->runtime);
        f->inject_initialized = false;
    }
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static void init_ready_coro(XrCoroutine *coro, int id, XrayIsolate *isolate) {
    memset(coro, 0, sizeof(*coro));
    coro->id = id;
    coro->isolate = isolate;
    atomic_store(&coro->flags, XR_CORO_FLG_READY);
    atomic_store(&coro->coro_state, XR_CORO_STATE_READY);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
}

TEST(local_runq_pops_recent_owner_items_first) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine a;
    XrCoroutine b;
    XrCoroutine c;
    init_ready_coro(&a, 101, &f.isolate_storage);
    init_ready_coro(&b, 102, &f.isolate_storage);
    init_ready_coro(&c, 103, &f.isolate_storage);

    xr_worker_push(&f.worker, &a);
    xr_worker_push(&f.worker, &b);
    xr_worker_push(&f.worker, &c);

    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &c);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &b);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &a);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), NULL);
    ASSERT_EQ_INT(xr_proc_local_runq_len(&f.worker.p), 0);

    scheduler_fixture_cleanup(&f);
}

TEST(lifo_budget_flushes_run_next_to_local_queue) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine coro;
    init_ready_coro(&coro, 201, &f.isolate_storage);

    xr_worker_push_lifo(&f.worker, &coro);
    f.worker.p.lifo_polls = XR_MAX_LIFO_POLLS;

    ASSERT_EQ_PTR(xr_worker_try_pop_lifo(&f.worker, true), NULL);
    ASSERT_EQ_INT((int) f.worker.p.stats.lifo_flush_count, 1);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &coro);

    scheduler_fixture_cleanup(&f);
}

TEST(global_inject_spill_preserves_all_work) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    enum {
        TOTAL = XR_LOCAL_QUEUE_SIZE + 20
    };
    XrCoroutine coros[TOTAL];
    for (int i = 0; i < TOTAL; i++) {
        init_ready_coro(&coros[i], 300 + i, &f.isolate_storage);
        xr_worker_push(&f.worker, &coros[i]);
    }

    int inject_len = atomic_load_explicit(&f.runtime.injectq.len, memory_order_relaxed);
    ASSERT_TRUE(inject_len > 0);
    ASSERT_TRUE(atomic_load_explicit(&f.runtime.injectq_nonempty, memory_order_acquire));

    int pulled = worker_pull_inject(&f.worker, XR_INJECT_POP_BATCH);
    ASSERT_TRUE(pulled > 0);
    ASSERT_TRUE(xr_proc_local_runq_len(&f.worker.p) > 0);

    scheduler_fixture_cleanup(&f);
}

TEST(work_stealing_moves_batch_and_returns_direct_item) {
    StealFixture f;
    ASSERT_TRUE(steal_fixture_init(&f));

    enum {
        TOTAL = 16
    };
    XrCoroutine coros[TOTAL];
    for (int i = 0; i < TOTAL; i++) {
        init_ready_coro(&coros[i], 400 + i, &f.isolate_storage);
        xr_worker_push(&f.workers[0], &coros[i]);
    }
    int64_t old_submit_time = xr_monotonic_ticks() - XR_STEAL_TIME_RESOLUTION_MS - 1;
    for (int i = 0; i < TOTAL; i++) {
        coros[i].submit_time = old_submit_time;
    }
    xr_worker_refresh_runq_masks(&f.workers[0]);

    int64_t delay = 0;
    bool should_exit = false;
    XrCoroutine *stolen = xr_worker_try_steal_once(&f.workers[1], &f.runtime, &f.runtime.running,
                                                   &delay, &should_exit);

    ASSERT_TRUE(!should_exit);
    ASSERT_TRUE(stolen != NULL);
    ASSERT_TRUE((int) f.workers[1].p.stats.steal_success_count > 0);
    ASSERT_TRUE((int) f.workers[1].p.stats.stolen_count > 0);
    ASSERT_TRUE(xr_proc_local_runq_len(&f.workers[0].p) < TOTAL);

    steal_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Scheduler Run Queue");
RUN_TEST(local_runq_pops_recent_owner_items_first);
RUN_TEST(lifo_budget_flushes_run_next_to_local_queue);
RUN_TEST(global_inject_spill_preserves_all_work);
RUN_TEST(work_stealing_moves_batch_and_returns_direct_item);

TEST_MAIN_END()
