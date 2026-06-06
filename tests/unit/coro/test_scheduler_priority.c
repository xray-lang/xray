/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_scheduler_priority.c - Unit tests for scheduler priority fairness
 */

#include "../test_framework.h"
#include "base/xconstants.h"
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

static bool scheduler_fixture_init(SchedulerFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->isolate_storage.sys_heap = &f->sys_heap;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    f->runtime.isolate = &f->isolate_storage;
    f->runtime.worker_count = 1;
    f->runtime.workers = &f->worker;
    f->runtime.machines = &f->machine;
    atomic_store(&f->runtime.running, true);
    atomic_store(&f->runtime.threads_started, false);
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

static void init_ready_coro(XrCoroutine *coro, int id, int priority, XrayIsolate *isolate) {
    memset(coro, 0, sizeof(*coro));
    coro->id = id;
    coro->isolate = isolate;
    atomic_store(&coro->flags, (uint32_t) priority);
    atomic_store(&coro->coro_state, XR_CORO_STATE_READY);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
}

TEST(weighted_priority_budget_dispatches_without_starving_lower_queues) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine high[XR_PRIO_WEIGHT_HIGH + 1];
    XrCoroutine normal[XR_PRIO_WEIGHT_NORMAL];
    XrCoroutine low;

    for (int i = 0; i < XR_PRIO_WEIGHT_HIGH + 1; i++) {
        init_ready_coro(&high[i], 100 + i, XR_CORO_PRIO_HIGH, &f.isolate_storage);
        xr_worker_push(&f.worker, &high[i]);
    }
    for (int i = 0; i < XR_PRIO_WEIGHT_NORMAL; i++) {
        init_ready_coro(&normal[i], 200 + i, XR_CORO_PRIO_NORMAL, &f.isolate_storage);
        xr_worker_push(&f.worker, &normal[i]);
    }
    init_ready_coro(&low, 300, XR_CORO_PRIO_LOW, &f.isolate_storage);
    xr_worker_push(&f.worker, &low);

    for (int i = 0; i < XR_PRIO_WEIGHT_HIGH; i++) {
        ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &high[XR_PRIO_WEIGHT_HIGH - i]);
    }
    for (int i = 0; i < XR_PRIO_WEIGHT_NORMAL; i++) {
        ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &normal[XR_PRIO_WEIGHT_NORMAL - 1 - i]);
    }
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &low);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &high[0]);
    ASSERT_NULL(xr_worker_pop(&f.worker));

    scheduler_fixture_cleanup(&f);
}

TEST(global_inject_preserves_runnable_age_for_priority_aging) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine old_low;
    XrCoroutine fresh_normal;
    init_ready_coro(&old_low, 500, XR_CORO_PRIO_LOW, &f.isolate_storage);
    init_ready_coro(&fresh_normal, 501, XR_CORO_PRIO_NORMAL, &f.isolate_storage);

    xr_injectq_push(&f.runtime, &old_low);
    xr_injectq_push(&f.runtime, &fresh_normal);

    old_low.submit_time =
        xr_monotonic_ticks() - (int64_t) (XR_PRIO_AGING_MS * XR_PRIO_AGING_MAX_BOOST + 1);

    ASSERT_EQ_INT(worker_pull_inject(&f.worker, XR_INJECT_POP_BATCH), 2);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &old_low);
    ASSERT_EQ_INT((int) f.worker.p.stats.priority_boost_count, 1);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &fresh_normal);
    ASSERT_NULL(xr_worker_pop(&f.worker));

    scheduler_fixture_cleanup(&f);
}

TEST(aging_boosts_old_low_priority_work_before_fresh_normal_work) {
    SchedulerFixture f;
    ASSERT_TRUE(scheduler_fixture_init(&f));

    XrCoroutine old_low;
    XrCoroutine fresh_normal;
    init_ready_coro(&old_low, 400, XR_CORO_PRIO_LOW, &f.isolate_storage);
    init_ready_coro(&fresh_normal, 401, XR_CORO_PRIO_NORMAL, &f.isolate_storage);

    xr_worker_push(&f.worker, &old_low);
    xr_worker_push(&f.worker, &fresh_normal);

    int64_t old_time =
        xr_monotonic_ticks() - (int64_t) (XR_PRIO_AGING_MS * XR_PRIO_AGING_MAX_BOOST + 1);
    old_low.submit_time = old_time;

    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &old_low);
    ASSERT_EQ_INT((int) f.worker.p.stats.priority_boost_count, 1);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &fresh_normal);
    ASSERT_NULL(xr_worker_pop(&f.worker));

    scheduler_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Scheduler Priority");
RUN_TEST(weighted_priority_budget_dispatches_without_starving_lower_queues);
RUN_TEST(global_inject_preserves_runnable_age_for_priority_aging);
RUN_TEST(aging_boosts_old_low_priority_work_before_fresh_normal_work);

TEST_MAIN_END()
