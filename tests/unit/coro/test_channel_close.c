/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_channel_close.c - Unit tests for channel close wake fan-out
 */

#include "../test_framework.h"
#include "coro/xchannel.h"
#include "coro/xcoroutine.h"
#include "coro/xworker_internal.h"
#include "coro/xyieldable.h"
#include "runtime/gc/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

typedef struct CloseFixture {
    XrayIsolate isolate_storage;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker worker;
    XrMachine machine;
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    bool sys_heap_initialized;
    bool worker_initialized;
} CloseFixture;

#define WAKE_ROUTE_WORKERS 3

typedef struct WakeRouteFixture {
    XrayIsolate isolate_storage;
    XrSystemHeap sys_heap;
    XrRuntime runtime;
    XrWorker workers[WAKE_ROUTE_WORKERS];
    XrMachine machines[WAKE_ROUTE_WORKERS];
    XrWorker *saved_worker;
    XrMachine *saved_machine;
    int initialized_workers;
    bool sys_heap_initialized;
} WakeRouteFixture;

static bool close_fixture_init(CloseFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL)) {
        return false;
    }
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

    xr_worker_init(&f->worker, 0, &f->runtime);
    f->worker_initialized = true;

    f->isolate_storage.vm.runtime = &f->runtime;
    tls_current_worker = &f->worker;
    tls_current_machine = &f->machine;
    return true;
}

static void close_fixture_cleanup(CloseFixture *f) {
    if (f->worker_initialized) {
        xr_worker_destroy(&f->worker);
        f->worker_initialized = false;
    }
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static bool wake_route_fixture_init(WakeRouteFixture *f) {
    memset(f, 0, sizeof(*f));

    if (!xr_sysheap_init(&f->sys_heap, NULL)) {
        return false;
    }
    f->sys_heap_initialized = true;
    f->isolate_storage.sys_heap = &f->sys_heap;

    f->saved_worker = tls_current_worker;
    f->saved_machine = tls_current_machine;

    f->runtime.isolate = &f->isolate_storage;
    f->runtime.worker_count = WAKE_ROUTE_WORKERS;
    f->runtime.workers = f->workers;
    f->runtime.machines = f->machines;
    f->runtime.sched_stats_enabled = true;
    atomic_store(&f->runtime.running, true);
    atomic_store(&f->runtime.threads_started, false);

    for (int i = 0; i < WAKE_ROUTE_WORKERS; i++) {
        xr_worker_init(&f->workers[i], i, &f->runtime);
        f->initialized_workers++;
    }

    f->isolate_storage.vm.runtime = &f->runtime;
    tls_current_worker = &f->workers[0];
    tls_current_machine = &f->machines[0];
    return true;
}

static void wake_route_fixture_cleanup(WakeRouteFixture *f) {
    for (int i = f->initialized_workers - 1; i >= 0; i--) {
        xr_worker_destroy(&f->workers[i]);
    }
    f->initialized_workers = 0;
    tls_current_worker = f->saved_worker;
    tls_current_machine = f->saved_machine;
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static bool wake_queue_has_pending(XrWorker *worker) {
    XrChanWakeCmdQueue *q = &worker->p.chan_wake_queue;
    XrChanWakeCmd *head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (!head)
        return false;
    XrChanWakeCmd *next =
        atomic_load_explicit((_Atomic(XrChanWakeCmd *) *) &head->next, memory_order_acquire);
    return next != NULL;
}

TEST(channel_close_wakes_select_waiter_without_caller_fanout) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine coro;
    memset(&coro, 0, sizeof(coro));
    coro.id = 7;
    coro.isolate = &f.isolate_storage;
    atomic_store(&coro.flags, XR_CORO_WAIT_SELECT | XR_CORO_FLG_BLOCKED | XR_CORO_PRIO_NORMAL);
    atomic_store(&coro.coro_state, XR_CORO_STATE_BLOCKED);
    atomic_store(&coro.resume_status, XR_RESUME_OK);
    atomic_store(&coro.affinity_p, 0);

    XrCoroExt ext;
    memset(&ext, 0, sizeof(ext));
    coro.ext = &ext;

    XrSelectCase *cases = ext.select_storage.inline_cases;
    XrSelectWait *wait = &ext.select_storage.wait;
    cases[0].channel = ch;
    cases[0].owner = &coro;
    wait->cases = cases;
    wait->case_count = 1;
    atomic_store(&wait->active, true);
    atomic_store(&wait->triggered, false);

    xr_worker_block_select(&f.worker, &coro, NULL, 1);
    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 1);
    ASSERT_NE(atomic_load(&ch->waiter_worker_mask), 0);

    xr_channel_close(ch);

    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 0);
    ASSERT_TRUE(atomic_load(&wait->triggered));
    ASSERT_EQ_INT(xr_coro_resume_load(&coro), XR_RESUME_CHANNEL_CLOSED);
    ASSERT_TRUE(xr_coro_flags_has(&coro, XR_CORO_FLG_READY));
    ASSERT_FALSE(xr_coro_flags_has(&coro, XR_CORO_FLG_BLOCKED));
    ASSERT_NULL(worker_blocked_bucket_find(&f.worker, ch));
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &coro);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_ready_wake_dispatches_single_remote_worker) {
    WakeRouteFixture f;
    ASSERT_TRUE(wake_route_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 1);
    ASSERT_NOT_NULL(ch);

    uint64_t remote_waiters = ((uint64_t) 1 << 1) | ((uint64_t) 1 << 2);
    atomic_store_explicit(&ch->waiter_worker_mask, remote_waiters, memory_order_release);

    ASSERT_NULL(xr_runtime_wake_channel(&f.isolate_storage, ch, false));

    ASSERT_EQ_INT((int) xr_sched_metric_load(&f.runtime.sched_stats.chan_wake_cmd_dispatch_count),
                  1);

    int queued_workers = 0;
    for (int i = 1; i < WAKE_ROUTE_WORKERS; i++) {
        if (wake_queue_has_pending(&f.workers[i])) {
            queued_workers++;
        }
    }
    ASSERT_EQ_INT(queued_workers, 1);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    wake_route_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Channel Close");
RUN_TEST(channel_close_wakes_select_waiter_without_caller_fanout);
RUN_TEST(channel_ready_wake_dispatches_single_remote_worker);

TEST_MAIN_END()
