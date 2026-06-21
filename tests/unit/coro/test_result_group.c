/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_result_group.c - Unit tests for ResultGroup reducer lifecycle
 */

#include "../test_framework.h"
#include "coro/xcoro_flags.h"
#include "coro/xcoroutine.h"
#include "coro/xresult_group.h"
#include "coro/xworker.h"
#include "runtime/gc/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

typedef struct ResultGroupFixture {
    XrayIsolate isolate_storage;
    XrRuntimeCore core;
    XrSystemHeap sys_heap;
    bool sys_heap_initialized;
} ResultGroupFixture;

static bool result_group_fixture_init(ResultGroupFixture *f) {
    memset(f, 0, sizeof(*f));
    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->core.sys_heap = &f->sys_heap;
    f->isolate_storage.core_rt = &f->core;
    return true;
}

static void result_group_fixture_cleanup(ResultGroupFixture *f) {
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static void init_blocked_result_group_coro(XrCoroutine *coro, XrCoroExt *ext, XrayIsolate *isolate,
                                           XrResultGroup *g) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = 710;
    coro->isolate = isolate;
    coro->core = isolate ? isolate->core_rt : NULL;
    coro->scheduler =
        (isolate && isolate->scheduler_runtime) ? (XrRuntime *) isolate->scheduler_runtime : NULL;
    coro->ext = ext;
    atomic_store(&coro->flags, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_RESULTGROUP);
    atomic_store(&coro->affinity_p, 0);

    xr_result_group_wait_token_prepare(&ext->wait.result_group_token, g);
    xr_result_group_wait_token_commit(&ext->wait.result_group_token);
}

TEST(batch_add_flush_and_recv_tracks_counts) {
    ResultGroupFixture f;
    ASSERT_TRUE(result_group_fixture_init(&f));

    XrResultGroup *g = xr_result_group_new(&f.isolate_storage, 3);
    ASSERT_NOT_NULL(g);

    ASSERT_TRUE(xr_result_group_add(g, 1));
    ASSERT_TRUE(xr_result_group_add(g, 2));
    ASSERT_EQ_INT((int) xr_result_group_length(g), 0);
    ASSERT_EQ_INT((int) xr_result_group_pending_count(g), 2);

    ASSERT_TRUE(xr_result_group_add(g, 3));
    ASSERT_EQ_INT((int) xr_result_group_length(g), 1);
    ASSERT_EQ_INT((int) xr_result_group_pending_count(g), 3);

    int64_t value = 0;
    ASSERT_TRUE(xr_result_group_try_recv(g, &value));
    ASSERT_EQ_INT((int) value, 6);
    ASSERT_EQ_INT((int) xr_result_group_length(g), 0);
    ASSERT_EQ_INT((int) xr_result_group_pending_count(g), 0);

    xr_gc_destroy_result_group(&g->gc, NULL);
    result_group_fixture_cleanup(&f);
}

TEST(close_flushes_partial_batch) {
    ResultGroupFixture f;
    ASSERT_TRUE(result_group_fixture_init(&f));

    XrResultGroup *g = xr_result_group_new(&f.isolate_storage, 4);
    ASSERT_NOT_NULL(g);

    ASSERT_TRUE(xr_result_group_add(g, 10));
    ASSERT_TRUE(xr_result_group_add(g, 7));
    xr_result_group_close(g);
    ASSERT_TRUE(xr_result_group_is_closed(g));
    ASSERT_EQ_INT((int) xr_result_group_length(g), 1);

    int64_t value = 0;
    ASSERT_TRUE(xr_result_group_try_recv(g, &value));
    ASSERT_EQ_INT((int) value, 17);
    ASSERT_FALSE(xr_result_group_try_recv(g, &value));

    xr_gc_destroy_result_group(&g->gc, NULL);
    result_group_fixture_cleanup(&f);
}

TEST(sched_stats_track_batch_lifecycle) {
    ResultGroupFixture f;
    ASSERT_TRUE(result_group_fixture_init(&f));

    XrRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.core = &f.core;
    xr_scheduler_runtime_attach_isolate(&runtime, &f.isolate_storage);
    runtime.sched_stats_enabled = true;
    f.isolate_storage.scheduler_runtime = &runtime;

    XrResultGroup *g = xr_result_group_new(&f.isolate_storage, 2);
    ASSERT_NOT_NULL(g);

    ASSERT_TRUE(xr_result_group_add(g, 11));
    ASSERT_TRUE(xr_result_group_add(g, 31));

    int64_t value = 0;
    ASSERT_TRUE(xr_result_group_try_recv(g, &value));
    ASSERT_EQ_INT(value, 42);
    ASSERT_FALSE(xr_result_group_try_recv(g, &value));

    ASSERT_TRUE(xr_result_group_add(g, 5));
    xr_result_group_close(g);
    ASSERT_TRUE(xr_result_group_try_recv(g, &value));
    ASSERT_EQ_INT(value, 5);

    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_add_count), 3);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_flush_count), 2);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_flush_item_count), 3);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_recv_count), 2);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_recv_empty_count), 1);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_block_count), 0);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_wake_count), 0);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_close_count), 1);
    ASSERT_EQ_UINT(xr_sched_metric_load(&runtime.sched_stats.result_group_close_wake_count), 0);

    xr_gc_destroy_result_group(&g->gc, NULL);
    result_group_fixture_cleanup(&f);
}

TEST(cancel_waiter_unlinks_coroutine_from_result_group) {
    ResultGroupFixture f;
    ASSERT_TRUE(result_group_fixture_init(&f));

    XrResultGroup *g = xr_result_group_new(&f.isolate_storage, 2);
    ASSERT_NOT_NULL(g);

    XrCoroutine coro;
    XrCoroExt ext;
    init_blocked_result_group_coro(&coro, &ext, &f.isolate_storage, g);

    g->wait_first = &coro;
    g->wait_last = &coro;
    atomic_store(&g->waiter_count, 1);

    xr_result_group_cancel_waiter(&coro);

    ASSERT_NULL(g->wait_first);
    ASSERT_NULL(g->wait_last);
    ASSERT_NULL(ext.wait_link);
    ASSERT_NULL(ext.wait_prev);
    ASSERT_EQ_INT((int) atomic_load(&g->waiter_count), 0);
    ASSERT_EQ_INT(atomic_load(&ext.wait.result_group_token.state), XR_RESULT_GROUP_WAIT_CANCELLED);

    xr_gc_destroy_result_group(&g->gc, NULL);
    result_group_fixture_cleanup(&f);
}

TEST(close_without_workers_keeps_waiter_blocked) {
    ResultGroupFixture f;
    ASSERT_TRUE(result_group_fixture_init(&f));

    XrRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.core = &f.core;
    xr_scheduler_runtime_attach_isolate(&runtime, &f.isolate_storage);
    runtime.worker_count = 0;
    runtime.workers = NULL;
    f.isolate_storage.scheduler_runtime = &runtime;

    XrResultGroup *g = xr_result_group_new(&f.isolate_storage, 2);
    ASSERT_NOT_NULL(g);

    XrCoroutine coro;
    XrCoroExt ext;
    init_blocked_result_group_coro(&coro, &ext, &f.isolate_storage, g);

    g->wait_first = &coro;
    g->wait_last = &coro;
    atomic_store(&g->waiter_count, 1);

    xr_result_group_close(g);

    ASSERT_TRUE(xr_result_group_is_closed(g));
    ASSERT_EQ_PTR(g->wait_first, &coro);
    ASSERT_EQ_PTR(g->wait_last, &coro);
    ASSERT_EQ_INT((int) atomic_load(&g->waiter_count), 1);
    ASSERT_EQ_INT(xr_flag_to_state(atomic_load(&coro.flags)), XR_CORO_STATE_BLOCKED);
    ASSERT_FALSE(xr_coro_flags_has(&coro, XR_CORO_FLG_READY));
    ASSERT_EQ_INT(atomic_load(&ext.wait.result_group_token.state), XR_RESULT_GROUP_WAIT_REGISTERED);

    g->wait_first = NULL;
    g->wait_last = NULL;
    atomic_store(&g->waiter_count, 0);
    xr_gc_destroy_result_group(&g->gc, NULL);
    result_group_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("ResultGroup");
RUN_TEST(batch_add_flush_and_recv_tracks_counts);
RUN_TEST(close_flushes_partial_batch);
RUN_TEST(sched_stats_track_batch_lifecycle);
RUN_TEST(cancel_waiter_unlinks_coroutine_from_result_group);
RUN_TEST(close_without_workers_keeps_waiter_blocked);

TEST_MAIN_END()
