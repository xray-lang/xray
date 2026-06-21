/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_work_queue_wait.c - Unit tests for WorkQueue waiter lifecycle
 */

#include "../test_framework.h"
#include "coro/xcoroutine.h"
#include "coro/xworker.h"
#include "coro/xwork_queue.h"
#include "runtime/gc/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include <stdatomic.h>
#include <string.h>

typedef struct WorkQueueFixture {
    XrayIsolate isolate_storage;
    XrRuntimeCore core;
    XrSystemHeap sys_heap;
    bool sys_heap_initialized;
} WorkQueueFixture;

static bool work_queue_fixture_init(WorkQueueFixture *f) {
    memset(f, 0, sizeof(*f));
    if (!xr_sysheap_init(&f->sys_heap, NULL))
        return false;
    f->sys_heap_initialized = true;
    f->core.sys_heap = &f->sys_heap;
    f->isolate_storage.core_rt = &f->core;
    return true;
}

static void work_queue_fixture_cleanup(WorkQueueFixture *f) {
    if (f->sys_heap_initialized) {
        xr_sysheap_destroy(&f->sys_heap);
        f->sys_heap_initialized = false;
    }
}

static void init_blocked_work_queue_coro(XrCoroutine *coro, XrCoroExt *ext, XrayIsolate *isolate,
                                         XrWorkQueue *q) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = 700;
    coro->isolate = isolate;
    coro->ext = ext;
    atomic_store(&coro->flags, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_WORKQUEUE);
    atomic_store(&coro->affinity_p, 0);

    xr_work_queue_wait_token_prepare(&ext->wait.work_queue_token, q);
    xr_work_queue_wait_token_commit(&ext->wait.work_queue_token);
}

TEST(cancel_waiter_unlinks_coroutine_from_work_queue) {
    WorkQueueFixture f;
    ASSERT_TRUE(work_queue_fixture_init(&f));

    XrWorkQueue *q = xr_work_queue_new(&f.isolate_storage, 1, 1);
    ASSERT_NOT_NULL(q);

    XrCoroutine coro;
    XrCoroExt ext;
    init_blocked_work_queue_coro(&coro, &ext, &f.isolate_storage, q);

    q->wait_first = &coro;
    q->wait_last = &coro;
    atomic_store(&q->waiter_count, 1);

    xr_work_queue_cancel_waiter(&coro);

    ASSERT_NULL(q->wait_first);
    ASSERT_NULL(q->wait_last);
    ASSERT_NULL(ext.wait_link);
    ASSERT_NULL(ext.wait_prev);
    ASSERT_EQ_INT((int) atomic_load(&q->waiter_count), 0);
    ASSERT_EQ_INT(atomic_load(&ext.wait.work_queue_token.state), XR_WORK_QUEUE_WAIT_CANCELLED);

    xr_gc_destroy_work_queue(&q->gc, NULL);
    work_queue_fixture_cleanup(&f);
}

TEST(close_without_workers_keeps_waiter_blocked) {
    WorkQueueFixture f;
    ASSERT_TRUE(work_queue_fixture_init(&f));

    XrRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.isolate = &f.isolate_storage;
    runtime.worker_count = 0;
    runtime.workers = NULL;
    f.isolate_storage.vm.runtime = &runtime;

    XrWorkQueue *q = xr_work_queue_new(&f.isolate_storage, 1, 1);
    ASSERT_NOT_NULL(q);

    XrCoroutine coro;
    XrCoroExt ext;
    init_blocked_work_queue_coro(&coro, &ext, &f.isolate_storage, q);

    q->wait_first = &coro;
    q->wait_last = &coro;
    atomic_store(&q->waiter_count, 1);

    xr_work_queue_close(q);

    ASSERT_TRUE(xr_work_queue_is_closed(q));
    ASSERT_EQ_PTR(q->wait_first, &coro);
    ASSERT_EQ_PTR(q->wait_last, &coro);
    ASSERT_EQ_INT((int) atomic_load(&q->waiter_count), 1);
    ASSERT_EQ_INT(xr_flag_to_state(atomic_load(&coro.flags)), XR_CORO_STATE_BLOCKED);
    ASSERT_FALSE(xr_coro_flags_has(&coro, XR_CORO_FLG_READY));
    ASSERT_EQ_INT(atomic_load(&ext.wait.work_queue_token.state), XR_WORK_QUEUE_WAIT_REGISTERED);

    q->wait_first = NULL;
    q->wait_last = NULL;
    atomic_store(&q->waiter_count, 0);
    xr_gc_destroy_work_queue(&q->gc, NULL);
    work_queue_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("WorkQueue Wait");
RUN_TEST(cancel_waiter_unlinks_coroutine_from_work_queue);
RUN_TEST(close_without_workers_keeps_waiter_blocked);

TEST_MAIN_END()
