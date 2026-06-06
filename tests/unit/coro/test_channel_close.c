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
#include "coro/xblock.h"
#include "coro/xchannel.h"
#include "coro/xcoroutine.h"
#include "coro/xnetpoll.h"
#include "coro/xtask.h"
#include "coro/xworker_internal.h"
#include "coro/xyieldable.h"
#include "runtime/object/xarray.h"
#include "runtime/gc/xsystem_heap.h"
#include "runtime/xisolate_internal.h"
#include "base/xmalloc.h"
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

static void init_stack_task(XrTask *task, uint8_t state, XrValue result, XrValue error) {
    memset(task, 0, sizeof(*task));
    xr_gc_header_init_type(&task->gc, XR_TTASK);
    task->result = result;
    task->error = error;
    atomic_store_explicit(&task->state, state, memory_order_relaxed);
    atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
    task->waiter_index = -1;
    task->waiter = NULL;
    atomic_init(&task->await_lock, false);
    task->await_waiters = NULL;
}

static bool init_task_array(XrArray *tasks, int capacity) {
    memset(tasks, 0, sizeof(*tasks));
    xr_gc_header_init_type(&tasks->gc, XR_TARRAY);
    xr_array_init_inplace(tasks, capacity, XR_ELEM_ANY);
    return tasks->data != NULL;
}

static void destroy_task_array(XrArray *tasks) {
    if (tasks->data && !tasks->data_on_gc_heap) {
        xr_free(tasks->data);
    }
    memset(tasks, 0, sizeof(*tasks));
}

static void init_blocked_channel_coro(XrCoroutine *coro, XrCoroExt *ext, int id,
                                      XrayIsolate *isolate, XrChannel *ch, bool wait_send) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = id;
    coro->isolate = isolate;
    coro->ext = ext;
    uint32_t wait_flag = wait_send ? XR_CORO_WAIT_CHANNEL_SEND : XR_CORO_WAIT_CHANNEL_RECV;
    atomic_store(&coro->flags, XR_CORO_FLG_BLOCKED | wait_flag);
    atomic_store(&coro->coro_state, XR_CORO_STATE_BLOCKED);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
    xr_channel_wait_token_prepare(&ext->chan_wait_token, ch, wait_send);
    xr_channel_wait_token_commit(&ext->chan_wait_token);
    atomic_store_explicit(&ext->wait_channel, ch, memory_order_release);
    ext->wait_send = wait_send;
    ext->wait_bucket_owner = -1;
}

static void init_blocked_io_coro(XrCoroutine *coro, XrCoroExt *ext, int id, XrayIsolate *isolate,
                                 int fd, int events, int64_t timeout_ms) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = id;
    coro->isolate = isolate;
    coro->ext = ext;
    atomic_store(&coro->flags, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_IO);
    atomic_store(&coro->coro_state, XR_CORO_STATE_BLOCKED);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
    xr_io_wait_token_prepare(&ext->wait.io_token, fd, events, 0, timeout_ms);
    xr_io_wait_token_commit(&ext->wait.io_token);
}

static void init_select_recv_coro(XrCoroutine *coro, XrCoroExt *ext, int id, XrayIsolate *isolate,
                                  XrChannel *ch) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = id;
    coro->isolate = isolate;
    coro->ext = ext;
    atomic_store(&coro->flags, XR_CORO_WAIT_SELECT | XR_CORO_FLG_BLOCKED);
    atomic_store(&coro->coro_state, XR_CORO_STATE_BLOCKED);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);

    XrSelectCase *cases = ext->select_storage.inline_cases;
    XrSelectWait *wait = &ext->select_storage.wait;
    cases[0].channel = ch;
    cases[0].owner = coro;
    wait->cases = cases;
    wait->case_count = 1;
    atomic_store(&wait->active, true);
    atomic_store(&wait->triggered, false);
    atomic_store(&wait->selected_index, -1);
    atomic_store(&wait->selected_status, 0);
    xr_select_wait_prepare(wait);
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
    atomic_store(&coro.flags, XR_CORO_WAIT_SELECT | XR_CORO_FLG_BLOCKED);
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
    xr_select_wait_prepare(wait);

    xr_worker_block_select(&f.worker, &coro, NULL, 1);
    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 1);
    ASSERT_NE(atomic_load(&ch->waiter_worker_mask), 0);
    ASSERT_EQ_INT(atomic_load(&wait->state), XR_SELECT_WAIT_REGISTERED);

    xr_channel_close(ch);

    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 0);
    ASSERT_TRUE(atomic_load(&wait->triggered));
    ASSERT_EQ_INT(atomic_load(&wait->state), XR_SELECT_WAIT_RESOLVED);
    ASSERT_EQ_INT(xr_coro_resume_load(&coro), XR_RESUME_CHANNEL_CLOSED);
    ASSERT_TRUE(xr_coro_flags_has(&coro, XR_CORO_FLG_READY));
    ASSERT_FALSE(xr_coro_flags_has(&coro, XR_CORO_FLG_BLOCKED));
    ASSERT_NULL(worker_blocked_bucket_find(&f.worker, ch));
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &coro);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_close_batches_select_waiter_wakes) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    enum {
        WAITER_COUNT = 5
    };
    XrCoroutine coros[WAITER_COUNT];
    XrCoroExt exts[WAITER_COUNT];

    for (int i = 0; i < WAITER_COUNT; i++) {
        init_select_recv_coro(&coros[i], &exts[i], 20 + i, &f.isolate_storage, ch);
        xr_worker_block_select(&f.worker, &coros[i], NULL, 1);
    }
    ASSERT_EQ_INT(f.worker.p.select_waiter_count, WAITER_COUNT);

    xr_channel_close(ch);

    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 0);
    ASSERT_NULL(worker_blocked_bucket_find(&f.worker, ch));

    bool popped[WAITER_COUNT] = {false};
    for (int i = 0; i < WAITER_COUNT; i++) {
        XrSelectWait *wait = &exts[i].select_storage.wait;
        ASSERT_TRUE(atomic_load(&wait->triggered));
        ASSERT_EQ_INT(atomic_load(&wait->state), XR_SELECT_WAIT_RESOLVED);
        ASSERT_EQ_INT(atomic_load(&wait->selected_index), 0);
        ASSERT_EQ_INT(atomic_load(&wait->selected_status), XR_RESUME_CHANNEL_CLOSED);
        ASSERT_EQ_INT(xr_coro_resume_load(&coros[i]), XR_RESUME_CHANNEL_CLOSED);
        ASSERT_TRUE(xr_coro_flags_has(&coros[i], XR_CORO_FLG_READY));
        ASSERT_FALSE(xr_coro_flags_has(&coros[i], XR_CORO_FLG_BLOCKED));
    }

    for (int i = 0; i < WAITER_COUNT; i++) {
        XrCoroutine *ready = xr_worker_pop(&f.worker);
        ASSERT_NOT_NULL(ready);
        int index = ready->id - 20;
        ASSERT_TRUE(index >= 0 && index < WAITER_COUNT);
        ASSERT_FALSE(popped[index]);
        popped[index] = true;
    }
    ASSERT_NULL(xr_worker_pop(&f.worker));

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(select_block_rechecks_already_closed_channel) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);
    xr_channel_close(ch);

    XrCoroutine coro;
    XrCoroExt ext;
    memset(&coro, 0, sizeof(coro));
    memset(&ext, 0, sizeof(ext));
    coro.id = 8;
    coro.isolate = &f.isolate_storage;
    coro.ext = &ext;
    atomic_store(&coro.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&coro.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&coro.resume_status, XR_RESUME_OK);
    atomic_store(&coro.affinity_p, 0);

    XrValue channel_values[1] = {xr_value_from_channel(ch)};
    XrCoroBlockResult result =
        xr_coro_select_block(&f.isolate_storage, &coro, channel_values, 1, NULL, 1);

    ASSERT_EQ_INT((int) result.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_TRUE(atomic_load(&ext.select_storage.wait.triggered));
    ASSERT_EQ_INT(atomic_load(&ext.select_storage.wait.state), XR_SELECT_WAIT_RESOLVED);
    ASSERT_EQ_INT(atomic_load(&ext.select_storage.wait.selected_index), 0);
    ASSERT_EQ_INT(atomic_load(&ext.select_storage.wait.selected_status), XR_RESUME_CHANNEL_CLOSED);
    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 0);
    ASSERT_FALSE(xr_coro_flags_has(&coro, XR_CORO_FLG_BLOCKED));
    ASSERT_NULL(worker_blocked_bucket_find(&f.worker, ch));

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(select_waiter_cancel_unlinks_worker_buckets) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 13;
    waiter.isolate = &f.isolate_storage;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);
    atomic_store(&waiter.affinity_p, 0);

    XrValue channels[1] = {xr_value_from_channel(ch)};
    XrCoroBlockResult blocked =
        xr_coro_select_block(&f.isolate_storage, &waiter, channels, 1, NULL, 1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 1);
    ASSERT_EQ_INT(f.worker.p.blocked_count, 1);
    XrBlockedBucket *bucket = worker_blocked_bucket_find(&f.worker, ch);
    ASSERT_NOT_NULL(bucket);
    ASSERT_NOT_NULL(bucket->select_head);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.select_storage.wait.state), XR_SELECT_WAIT_REGISTERED);

    xr_coro_cancel(&waiter);

    ASSERT_EQ_INT(f.worker.p.select_waiter_count, 0);
    ASSERT_EQ_INT(f.worker.p.blocked_count, 0);
    ASSERT_NULL(worker_blocked_bucket_find(&f.worker, ch));
    ASSERT_FALSE(atomic_load(&waiter_ext.select_storage.wait.active));
    ASSERT_EQ_INT(atomic_load(&waiter_ext.select_storage.wait.state), XR_SELECT_WAIT_CANCELLED);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_CANCELLED));
    ASSERT_NULL(xr_worker_pop(&f.worker));

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

TEST(channel_wake_command_batches_ready_waiters) {
    WakeRouteFixture f;
    ASSERT_TRUE(wake_route_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    enum {
        WAITER_COUNT = 3
    };
    XrCoroutine coros[WAITER_COUNT];
    XrCoroExt exts[WAITER_COUNT];
    for (int i = 0; i < WAITER_COUNT; i++) {
        init_blocked_channel_coro(&coros[i], &exts[i], 30 + i, &f.isolate_storage, ch, true);
        xr_worker_block(&f.workers[1], &coros[i]);
        xr_channel_note_waiter(ch, 1, true);
    }
    ASSERT_EQ_INT(f.workers[1].p.blocked_count, WAITER_COUNT);

    for (int i = 0; i < WAITER_COUNT; i++) {
        xr_worker_dispatch_chan_wake(&f.runtime, 1, ch, true, false);
    }
    ASSERT_TRUE(wake_queue_has_pending(&f.workers[1]));

    tls_current_worker = &f.workers[1];
    tls_current_machine = &f.machines[1];
    xr_worker_drain_chan_wake_queue(&f.workers[1]);

    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 0);
    ASSERT_NULL(worker_blocked_bucket_find(&f.workers[1], ch));
    ASSERT_EQ_INT((int) xr_sched_metric_load(&f.runtime.sched_stats.chan_wake_cmd_coalesce_count),
                  WAITER_COUNT - 1);

    bool popped[WAITER_COUNT] = {false};
    for (int i = 0; i < WAITER_COUNT; i++) {
        ASSERT_EQ_INT(xr_coro_resume_load(&coros[i]), XR_RESUME_CHANNEL);
        ASSERT_TRUE(xr_coro_flags_has(&coros[i], XR_CORO_FLG_READY));
        ASSERT_FALSE(xr_coro_flags_has(&coros[i], XR_CORO_FLG_BLOCKED));

        XrCoroutine *ready = xr_worker_pop(&f.workers[1]);
        ASSERT_NOT_NULL(ready);
        int index = ready->id - 30;
        ASSERT_TRUE(index >= 0 && index < WAITER_COUNT);
        ASSERT_FALSE(popped[index]);
        popped[index] = true;
    }
    ASSERT_NULL(xr_worker_pop(&f.workers[1]));

    tls_current_worker = &f.workers[0];
    tls_current_machine = &f.machines[0];
    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    wake_route_fixture_cleanup(&f);
}

TEST(channel_direction_masks_refresh_after_partial_wake) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine sender;
    XrCoroutine receiver;
    XrCoroExt sender_ext;
    XrCoroExt receiver_ext;
    init_blocked_channel_coro(&sender, &sender_ext, 11, &f.isolate_storage, ch, true);
    init_blocked_channel_coro(&receiver, &receiver_ext, 12, &f.isolate_storage, ch, false);

    xr_worker_block(&f.worker, &sender);
    xr_worker_block(&f.worker, &receiver);
    xr_channel_note_waiter(ch, f.worker.p.id, true);
    xr_channel_note_waiter(ch, f.worker.p.id, false);

    uint64_t bit = xr_channel_worker_bit(f.worker.p.id);
    ASSERT_TRUE((atomic_load(&ch->waiter_worker_mask) & bit) != 0);
    ASSERT_TRUE((atomic_load(&ch->sender_waiter_worker_mask) & bit) != 0);
    ASSERT_TRUE((atomic_load(&ch->receiver_waiter_worker_mask) & bit) != 0);

    ASSERT_EQ_PTR(xr_worker_wake_one(&f.worker, ch, true), &sender);
    ASSERT_TRUE((atomic_load(&ch->waiter_worker_mask) & bit) != 0);
    ASSERT_FALSE((atomic_load(&ch->sender_waiter_worker_mask) & bit) != 0);
    ASSERT_TRUE((atomic_load(&ch->receiver_waiter_worker_mask) & bit) != 0);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &sender);

    ASSERT_EQ_PTR(xr_worker_wake_one(&f.worker, ch, false), &receiver);
    ASSERT_FALSE((atomic_load(&ch->waiter_worker_mask) & bit) != 0);
    ASSERT_FALSE((atomic_load(&ch->sender_waiter_worker_mask) & bit) != 0);
    ASSERT_FALSE((atomic_load(&ch->receiver_waiter_worker_mask) & bit) != 0);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &receiver);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_shape_op_metrics_track_logical_and_worker_kinds) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));
    f.runtime.sched_stats_enabled = true;

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 4);
    ASSERT_NOT_NULL(ch);

    XrCoroutine producer;
    XrCoroutine consumer;
    memset(&producer, 0, sizeof(producer));
    memset(&consumer, 0, sizeof(consumer));
    producer.id = 101;
    consumer.id = 202;
    producer.isolate = &f.isolate_storage;
    consumer.isolate = &f.isolate_storage;

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(1), &producer, -1), (int) XR_CHAN_OK);
    XrValue out = xr_null();
    ASSERT_EQ_INT((int) xr_channel_recv(ch, &out, &consumer, -1), (int) XR_CHAN_OK);
    ASSERT_EQ_INT((int) XR_TO_INT(out), 1);

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(2), &producer, -1), (int) XR_CHAN_OK);
    ASSERT_EQ_INT((int) xr_channel_recv(ch, &out, &consumer, -1), (int) XR_CHAN_OK);
    ASSERT_EQ_INT((int) XR_TO_INT(out), 2);

    ASSERT_EQ_INT((int) xr_sched_metric_load(&f.runtime.sched_stats.chan_kind_generic_op_count), 1);
    ASSERT_EQ_INT((int) xr_sched_metric_load(&f.runtime.sched_stats.chan_kind_spsc_op_count), 3);
    ASSERT_EQ_INT(
        (int) xr_sched_metric_load(&f.runtime.sched_stats.chan_worker_kind_generic_op_count), 1);
    ASSERT_EQ_INT((int) xr_sched_metric_load(&f.runtime.sched_stats.chan_worker_kind_spsc_op_count),
                  3);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel) + 4 * sizeof(XrValue));
    close_fixture_cleanup(&f);
}

TEST(channel_wait_token_tracks_block_wake_and_resume) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine sender;
    XrCoroExt sender_ext;
    memset(&sender, 0, sizeof(sender));
    memset(&sender_ext, 0, sizeof(sender_ext));
    sender.id = 301;
    sender.isolate = &f.isolate_storage;
    sender.ext = &sender_ext;
    atomic_store(&sender.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&sender.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&sender.resume_status, XR_RESUME_OK);
    atomic_store(&sender.affinity_p, 0);

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(7), &sender, -1), (int) XR_CHAN_BLOCK);
    ASSERT_EQ_INT(atomic_load(&sender_ext.chan_wait_token.state), XR_CHAN_WAIT_REGISTERED);
    ASSERT_EQ_PTR(atomic_load(&sender_ext.chan_wait_token.channel), ch);
    ASSERT_TRUE(sender_ext.chan_wait_token.is_send);
    ASSERT_EQ_INT(atomic_load(&sender.coro_state), XR_CORO_STATE_BLOCKED);
    ASSERT_EQ_INT(xr_coro_get_wait_reason(xr_coro_flags_load(&sender)),
                  XR_CORO_WAIT_CHANNEL_SEND >> XR_CORO_WAIT_SHIFT);

    xr_channel_close(ch);
    ASSERT_EQ_INT(atomic_load(&sender_ext.chan_wait_token.state), XR_CHAN_WAIT_RESOLVED);
    ASSERT_EQ_INT(xr_coro_resume_load(&sender), XR_RESUME_CHANNEL_CLOSED);

    XrCoroBlockResult resumed = xr_coro_chan_send_resume(&sender, xr_slot_none());
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_CLOSED);
    ASSERT_EQ_INT(atomic_load(&sender_ext.chan_wait_token.state), XR_CHAN_WAIT_IDLE);
    ASSERT_NULL(atomic_load(&sender_ext.chan_wait_token.channel));

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_timed_wait_cancels_timer_on_channel_wake) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine sender;
    XrCoroExt sender_ext;
    memset(&sender, 0, sizeof(sender));
    memset(&sender_ext, 0, sizeof(sender_ext));
    sender.id = 302;
    sender.isolate = &f.isolate_storage;
    sender.ext = &sender_ext;
    atomic_store(&sender.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&sender.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&sender.resume_status, XR_RESUME_OK);
    atomic_store(&sender.affinity_p, 0);

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(7), &sender, 1000), (int) XR_CHAN_BLOCK);
    ASSERT_TRUE(atomic_load_explicit(&sender_ext.timer_active, memory_order_relaxed));
    ASSERT_EQ_INT(atomic_load(&sender_ext.wait.timer_token.state), XR_TIMER_WAIT_REGISTERED);

    XrCoroutine receiver;
    memset(&receiver, 0, sizeof(receiver));
    receiver.id = 303;
    receiver.isolate = &f.isolate_storage;
    atomic_store(&receiver.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&receiver.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&receiver.resume_status, XR_RESUME_OK);
    atomic_store(&receiver.affinity_p, 0);

    XrValue out = xr_null();
    ASSERT_EQ_INT((int) xr_channel_recv(ch, &out, &receiver, -1), (int) XR_CHAN_OK);
    ASSERT_EQ_INT((int) XR_TO_INT(out), 7);
    ASSERT_FALSE(atomic_load_explicit(&sender_ext.timer_active, memory_order_relaxed));
    ASSERT_EQ_INT(atomic_load(&sender_ext.wait.timer_token.state), XR_TIMER_WAIT_CANCELLED);
    ASSERT_EQ_INT(xr_coro_resume_load(&sender), XR_RESUME_CHANNEL);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_notify_send_without_current_worker_routes_to_inbox) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine receiver;
    XrCoroExt receiver_ext;
    memset(&receiver, 0, sizeof(receiver));
    memset(&receiver_ext, 0, sizeof(receiver_ext));
    receiver.id = 304;
    receiver.isolate = &f.isolate_storage;
    receiver.ext = &receiver_ext;
    atomic_store(&receiver.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&receiver.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&receiver.resume_status, XR_RESUME_OK);
    atomic_store(&receiver.affinity_p, 0);

    XrValue recv_value = xr_null();
    XrCoroBlockResult blocked = xr_coro_chan_recv(
        &f.isolate_storage, &receiver, ch, xr_slot_xvalue_ptr(&recv_value), xr_slot_none(), -1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&receiver_ext.chan_wait_token.state), XR_CHAN_WAIT_REGISTERED);
    ASSERT_EQ_PTR(ch->recvq.first, &receiver);

    tls_current_worker = NULL;
    tls_current_machine = NULL;
    ASSERT_TRUE(xr_channel_notify_send(ch, xr_int(44)));

    ASSERT_EQ_INT((int) atomic_load(&f.runtime.total_inbox_len), 1);
    ASSERT_EQ_INT(xr_coro_resume_load(&receiver), XR_RESUME_CHANNEL);
    ASSERT_EQ_INT(atomic_load(&receiver_ext.chan_wait_token.state), XR_CHAN_WAIT_RESOLVED);
    ASSERT_NULL(atomic_load(&receiver_ext.wait_channel));
    ASSERT_TRUE(xr_coro_flags_has(&receiver, XR_CORO_FLG_READY));
    ASSERT_FALSE(xr_coro_flags_has(&receiver, XR_CORO_FLG_BLOCKED));

    tls_current_worker = &f.worker;
    tls_current_machine = &f.machine;
    worker_drain_inbox(&f.worker);
    ASSERT_EQ_INT((int) atomic_load(&f.runtime.total_inbox_len), 0);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &receiver);

    XrCoroBlockResult resumed = xr_coro_chan_recv_resume(
        &f.isolate_storage, &receiver, xr_slot_xvalue_ptr(&recv_value), xr_slot_none());
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_EQ_INT((int) XR_TO_INT(recv_value), 44);
    ASSERT_EQ_INT(atomic_load(&receiver_ext.chan_wait_token.state), XR_CHAN_WAIT_IDLE);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_waiter_cancel_unlinks_channel_and_worker_queues) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine sender;
    XrCoroExt sender_ext;
    memset(&sender, 0, sizeof(sender));
    memset(&sender_ext, 0, sizeof(sender_ext));
    sender.id = 302;
    sender.isolate = &f.isolate_storage;
    sender.ext = &sender_ext;
    atomic_store(&sender.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&sender.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&sender.resume_status, XR_RESUME_OK);
    atomic_store(&sender.affinity_p, 0);

    XrValue send_ok = xr_null();
    XrCoroBlockResult blocked = xr_coro_chan_send(&f.isolate_storage, &sender, ch, xr_int(11),
                                                  xr_slot_xvalue_ptr(&send_ok), -1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_PTR(ch->sendq.first, &sender);
    ASSERT_EQ_PTR(ch->sendq.last, &sender);

    xr_worker_block(&f.worker, &sender);
    ASSERT_EQ_INT(f.worker.p.blocked_count, 1);
    ASSERT_NOT_NULL(sender_ext.wait_bucket);

    xr_coro_cancel(&sender);

    ASSERT_NULL(ch->sendq.first);
    ASSERT_NULL(ch->sendq.last);
    ASSERT_EQ_INT(f.worker.p.blocked_count, 0);
    ASSERT_NULL(worker_blocked_bucket_find(&f.worker, ch));
    ASSERT_NULL(sender_ext.chan_wait_queue);
    ASSERT_NULL(sender_ext.chan_wait_next);
    ASSERT_NULL(sender_ext.chan_wait_prev);
    ASSERT_NULL(sender_ext.wait_bucket);
    ASSERT_NULL(sender_ext.wait_link);
    ASSERT_NULL(sender_ext.wait_prev);
    ASSERT_NULL(atomic_load(&sender_ext.wait_channel));
    ASSERT_EQ_INT(atomic_load(&sender_ext.chan_wait_token.state), XR_CHAN_WAIT_CANCELLED);
    ASSERT_TRUE(xr_coro_flags_has(&sender, XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&sender, XR_CORO_FLG_CANCELLED));
    ASSERT_FALSE(xr_coro_flags_has(&sender, XR_CORO_FLG_BLOCKED));

    xr_channel_close(ch);
    ASSERT_NULL(xr_worker_pop(&f.worker));

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(channel_waiter_cancel_routes_foreign_owner_detach) {
    WakeRouteFixture f;
    ASSERT_TRUE(wake_route_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine sender;
    XrCoroExt sender_ext;
    memset(&sender, 0, sizeof(sender));
    memset(&sender_ext, 0, sizeof(sender_ext));
    sender.id = 303;
    sender.isolate = &f.isolate_storage;
    sender.ext = &sender_ext;
    atomic_store(&sender.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&sender.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&sender.resume_status, XR_RESUME_OK);
    atomic_store(&sender.affinity_p, 1);

    tls_current_worker = &f.workers[1];
    tls_current_machine = &f.machines[1];
    XrValue send_ok = xr_null();
    XrCoroBlockResult blocked = xr_coro_chan_send(&f.isolate_storage, &sender, ch, xr_int(12),
                                                  xr_slot_xvalue_ptr(&send_ok), -1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    xr_worker_block(&f.workers[1], &sender);
    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 1);
    ASSERT_EQ_INT(sender_ext.wait_bucket_owner, 1);

    tls_current_worker = &f.workers[0];
    tls_current_machine = &f.machines[0];
    xr_coro_cancel(&sender);

    ASSERT_NULL(ch->sendq.first);
    ASSERT_NULL(ch->sendq.last);
    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 1);
    ASSERT_NOT_NULL(sender_ext.wait_bucket);
    ASSERT_EQ_INT((int) atomic_load(&f.runtime.total_inbox_len), 1);

    tls_current_worker = &f.workers[1];
    tls_current_machine = &f.machines[1];
    worker_drain_inbox(&f.workers[1]);

    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 0);
    ASSERT_EQ_INT((int) atomic_load(&f.runtime.total_inbox_len), 0);
    ASSERT_NULL(worker_blocked_bucket_find(&f.workers[1], ch));
    ASSERT_NULL(sender_ext.wait_bucket);
    ASSERT_NULL(sender_ext.wait_link);
    ASSERT_NULL(sender_ext.wait_prev);
    ASSERT_NULL(xr_worker_pop(&f.workers[1]));
    ASSERT_TRUE(xr_coro_flags_has(&sender, XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&sender, XR_CORO_FLG_CANCELLED));

    tls_current_worker = &f.workers[0];
    tls_current_machine = &f.machines[0];
    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    wake_route_fixture_cleanup(&f);
}

TEST(select_waiter_cancel_routes_foreign_owner_detach) {
    WakeRouteFixture f;
    ASSERT_TRUE(wake_route_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 304;
    waiter.isolate = &f.isolate_storage;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);
    atomic_store(&waiter.affinity_p, 1);

    tls_current_worker = &f.workers[1];
    tls_current_machine = &f.machines[1];
    XrValue channels[1] = {xr_value_from_channel(ch)};
    XrCoroBlockResult blocked =
        xr_coro_select_block(&f.isolate_storage, &waiter, channels, 1, NULL, 1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(f.workers[1].p.select_waiter_count, 1);
    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 1);
    ASSERT_EQ_INT((int) atomic_load(&waiter.affinity_p), 1);

    tls_current_worker = &f.workers[0];
    tls_current_machine = &f.machines[0];
    xr_coro_cancel(&waiter);

    ASSERT_EQ_INT(f.workers[1].p.select_waiter_count, 1);
    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 1);
    ASSERT_TRUE(atomic_load(&waiter_ext.select_storage.wait.active));
    ASSERT_EQ_INT((int) atomic_load(&f.runtime.total_inbox_len), 1);

    tls_current_worker = &f.workers[1];
    tls_current_machine = &f.machines[1];
    worker_drain_inbox(&f.workers[1]);

    ASSERT_EQ_INT(f.workers[1].p.select_waiter_count, 0);
    ASSERT_EQ_INT(f.workers[1].p.blocked_count, 0);
    ASSERT_EQ_INT((int) atomic_load(&f.runtime.total_inbox_len), 0);
    ASSERT_NULL(worker_blocked_bucket_find(&f.workers[1], ch));
    ASSERT_FALSE(atomic_load(&waiter_ext.select_storage.wait.active));
    ASSERT_EQ_INT(atomic_load(&waiter_ext.select_storage.wait.state), XR_SELECT_WAIT_CANCELLED);
    ASSERT_NULL(xr_worker_pop(&f.workers[1]));
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_DONE));
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_CANCELLED));

    tls_current_worker = &f.workers[0];
    tls_current_machine = &f.machines[0];
    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    wake_route_fixture_cleanup(&f);
}

TEST(await_wait_token_tracks_register_resolve_and_resume) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    XrTask task;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    memset(&task, 0, sizeof(task));

    waiter.id = 401;
    waiter.isolate = &f.isolate_storage;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);
    atomic_store(&waiter.affinity_p, 0);

    task.result = xr_null();
    task.error = xr_null();
    atomic_store(&task.state, XR_TASK_ACTIVE);
    atomic_store(&task.await_state, XR_AWAIT_NONE);
    task.waiter_index = -1;
    task.waiter = NULL;
    atomic_init(&task.await_lock, false);
    task.await_waiters = NULL;

    XrCoroBlockResult blocked = xr_coro_await_task(&waiter, &task, -1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.await_token.state), XR_AWAIT_WAIT_REGISTERED);
    ASSERT_EQ_PTR(atomic_load(&waiter_ext.wait.await_token.task), &task);
    ASSERT_EQ_INT(waiter_ext.wait.await_token.waiter_index, -1);
    ASSERT_EQ_PTR(atomic_load(&waiter_ext.wait.await_task), &task);
    ASSERT_EQ_PTR(task.await_waiters, &waiter_ext.wait.await_token.node);
    ASSERT_EQ_PTR(waiter_ext.wait.await_token.node.waiter, &waiter);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_BLOCKED));
    ASSERT_EQ_INT(atomic_load(&waiter.coro_state), XR_CORO_STATE_BLOCKED);
    ASSERT_EQ_INT(xr_coro_get_wait_reason(xr_coro_flags_load(&waiter)),
                  XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);

    task.result = xr_int(9);
    atomic_store(&task.state, XR_TASK_COMPLETED);
    xr_task_wake_waiter(&f.isolate_storage, &task);

    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.await_token.state), XR_AWAIT_WAIT_RESOLVED);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_READY));
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &waiter);

    XrCoroBlockResult resumed = xr_coro_await_task_resume(&waiter, &task);
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_EQ_INT((int) XR_TO_INT(resumed.value), 9);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.await_token.state), XR_AWAIT_WAIT_IDLE);
    ASSERT_NULL(atomic_load(&waiter_ext.wait.await_token.task));
    ASSERT_NULL(atomic_load(&waiter_ext.wait.await_task));

    close_fixture_cleanup(&f);
}

TEST(single_task_await_wakes_multiple_waiters) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine first_waiter;
    XrCoroutine second_waiter;
    XrCoroExt first_ext;
    XrCoroExt second_ext;
    memset(&first_waiter, 0, sizeof(first_waiter));
    memset(&second_waiter, 0, sizeof(second_waiter));
    memset(&first_ext, 0, sizeof(first_ext));
    memset(&second_ext, 0, sizeof(second_ext));
    first_waiter.id = 421;
    second_waiter.id = 422;
    first_waiter.isolate = &f.isolate_storage;
    second_waiter.isolate = &f.isolate_storage;
    first_waiter.ext = &first_ext;
    second_waiter.ext = &second_ext;
    atomic_store(&first_waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&second_waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&first_waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&second_waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&first_waiter.resume_status, XR_RESUME_OK);
    atomic_store(&second_waiter.resume_status, XR_RESUME_OK);
    atomic_store(&first_waiter.affinity_p, 0);
    atomic_store(&second_waiter.affinity_p, 0);

    XrTask task;
    init_stack_task(&task, XR_TASK_ACTIVE, xr_null(), xr_null());

    XrCoroBlockResult first_blocked = xr_coro_await_task(&first_waiter, &task, -1);
    XrCoroBlockResult second_blocked = xr_coro_await_task(&second_waiter, &task, -1);
    ASSERT_EQ_INT((int) first_blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT((int) second_blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_NOT_NULL(task.await_waiters);
    ASSERT_EQ_INT(atomic_load(&first_ext.wait.await_token.state), XR_AWAIT_WAIT_REGISTERED);
    ASSERT_EQ_INT(atomic_load(&second_ext.wait.await_token.state), XR_AWAIT_WAIT_REGISTERED);
    ASSERT_TRUE(xr_coro_flags_has(&first_waiter, XR_CORO_FLG_BLOCKED));
    ASSERT_TRUE(xr_coro_flags_has(&second_waiter, XR_CORO_FLG_BLOCKED));
    ASSERT_EQ_INT(atomic_load(&first_waiter.coro_state), XR_CORO_STATE_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&second_waiter.coro_state), XR_CORO_STATE_BLOCKED);
    task.result = xr_int(77);
    atomic_store(&task.state, XR_TASK_COMPLETED);
    xr_task_wake_waiter(&f.isolate_storage, &task);

    ASSERT_NULL(task.await_waiters);
    ASSERT_EQ_INT(atomic_load(&first_ext.wait.await_token.state), XR_AWAIT_WAIT_RESOLVED);
    ASSERT_EQ_INT(atomic_load(&second_ext.wait.await_token.state), XR_AWAIT_WAIT_RESOLVED);
    ASSERT_TRUE(xr_coro_flags_has(&first_waiter, XR_CORO_FLG_READY));
    ASSERT_TRUE(xr_coro_flags_has(&second_waiter, XR_CORO_FLG_READY));

    XrCoroutine *popped_a = xr_worker_pop(&f.worker);
    XrCoroutine *popped_b = xr_worker_pop(&f.worker);
    ASSERT_TRUE((popped_a == &first_waiter && popped_b == &second_waiter) ||
                (popped_a == &second_waiter && popped_b == &first_waiter));

    XrCoroBlockResult first_resumed = xr_coro_await_task_resume(&first_waiter, &task);
    XrCoroBlockResult second_resumed = xr_coro_await_task_resume(&second_waiter, &task);
    ASSERT_EQ_INT((int) first_resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_EQ_INT((int) second_resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_EQ_INT(XR_TO_INT(first_resumed.value), 77);
    ASSERT_EQ_INT(XR_TO_INT(second_resumed.value), 77);
    ASSERT_EQ_INT(atomic_load(&first_ext.wait.await_token.state), XR_AWAIT_WAIT_IDLE);
    ASSERT_EQ_INT(atomic_load(&second_ext.wait.await_token.state), XR_AWAIT_WAIT_IDLE);

    close_fixture_cleanup(&f);
}

TEST(single_task_await_legacy_and_node_wake_claim_once) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 424;
    waiter.isolate = &f.isolate_storage;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);
    atomic_store(&waiter.affinity_p, 0);

    XrTask task;
    init_stack_task(&task, XR_TASK_ACTIVE, xr_null(), xr_null());

    XrCoroBlockResult blocked = xr_coro_await_task(&waiter, &task, -1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_PTR(task.await_waiters, &waiter_ext.wait.await_token.node);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_BLOCKED));
    ASSERT_EQ_INT(atomic_load(&waiter.coro_state), XR_CORO_STATE_BLOCKED);

    task.waiter = &waiter;
    task.waiter_index = -1;
    task.result = xr_int(33);
    atomic_store(&task.state, XR_TASK_COMPLETED);
    xr_task_wake_waiter(&f.isolate_storage, &task);

    ASSERT_NULL(task.await_waiters);
    ASSERT_NULL(task.waiter);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_READY));
    ASSERT_FALSE(xr_coro_flags_has(&waiter, XR_CORO_FLG_BLOCKED));
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &waiter);
    ASSERT_NULL(xr_worker_pop(&f.worker));

    XrCoroBlockResult resumed = xr_coro_await_task_resume(&waiter, &task);
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_EQ_INT((int) XR_TO_INT(resumed.value), 33);

    close_fixture_cleanup(&f);
}

TEST(single_task_await_timeout_unlinks_waiter_node) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 423;
    waiter.isolate = &f.isolate_storage;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);
    atomic_store(&waiter.affinity_p, 0);

    XrTask task;
    init_stack_task(&task, XR_TASK_ACTIVE, xr_null(), xr_null());

    XrCoroBlockResult blocked = xr_coro_await_task(&waiter, &task, -1);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_PTR(task.await_waiters, &waiter_ext.wait.await_token.node);
    ASSERT_EQ_PTR(atomic_load(&waiter_ext.wait.await_task), &task);

    xr_coro_resume_store(&waiter, XR_RESUME_TIMEOUT);
    XrCoroBlockResult resumed = xr_coro_await_task_resume(&waiter, &task);

    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_TIMEOUT);
    ASSERT_NULL(task.await_waiters);
    ASSERT_EQ_INT(atomic_load(&task.await_state), XR_AWAIT_NONE);
    ASSERT_NULL(atomic_load(&waiter_ext.wait.await_task));
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.await_token.state), XR_AWAIT_WAIT_IDLE);
    ASSERT_NULL(waiter_ext.wait.await_token.node.waiter);

    close_fixture_cleanup(&f);
}

TEST(timer_wait_token_tracks_channel_timeout_cancel_on_wake) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    ASSERT_NOT_NULL(ch);

    XrCoroutine sender;
    XrCoroExt sender_ext;
    memset(&sender, 0, sizeof(sender));
    memset(&sender_ext, 0, sizeof(sender_ext));
    sender.id = 501;
    sender.isolate = &f.isolate_storage;
    sender.ext = &sender_ext;
    atomic_store(&sender.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&sender.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&sender.resume_status, XR_RESUME_OK);
    atomic_store(&sender.affinity_p, 0);

    XrValue send_ok = xr_null();
    XrCoroBlockResult blocked = xr_coro_chan_send(&f.isolate_storage, &sender, ch, xr_int(7),
                                                  xr_slot_xvalue_ptr(&send_ok), 1000);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&sender_ext.wait.timer_token.state), XR_TIMER_WAIT_REGISTERED);
    ASSERT_EQ_INT(sender_ext.wait.timer_token.owner_worker_id, 0);
    ASSERT_EQ_INT(sender_ext.wait.timer_token.wait_reason,
                  XR_CORO_WAIT_CHANNEL_SEND >> XR_CORO_WAIT_SHIFT);
    ASSERT_TRUE(atomic_load(&sender_ext.timer_active));

    atomic_store(&sender.flags, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_CHANNEL_SEND);
    atomic_store(&sender.coro_state, XR_CORO_STATE_BLOCKED);
    xr_worker_block(&f.worker, &sender);

    xr_channel_close(ch);

    ASSERT_EQ_INT(atomic_load(&sender_ext.wait.timer_token.state), XR_TIMER_WAIT_CANCELLED);
    ASSERT_FALSE(atomic_load(&sender_ext.timer_active));
    ASSERT_EQ_INT(xr_coro_resume_load(&sender), XR_RESUME_CHANNEL_CLOSED);

    XrCoroBlockResult resumed = xr_coro_chan_send_resume(&sender, xr_slot_xvalue_ptr(&send_ok));
    ASSERT_EQ_INT((int) resumed.kind, (int) XR_CORO_BLOCK_CLOSED);
    ASSERT_EQ_INT(atomic_load(&sender_ext.wait.timer_token.state), XR_TIMER_WAIT_IDLE);

    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(timer_wait_token_cancels_select_timer_on_channel_wake) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrChannel *ch = xr_channel_new(&f.isolate_storage, 0);
    XrChannel *timer_ch = xr_channel_new_timer(&f.isolate_storage, 1000);
    ASSERT_NOT_NULL(ch);
    ASSERT_NOT_NULL(timer_ch);

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 502;
    waiter.isolate = &f.isolate_storage;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);
    atomic_store(&waiter.affinity_p, 0);

    XrValue channels[2] = {xr_value_from_channel(ch), xr_value_from_channel(timer_ch)};
    XrCoroBlockResult blocked =
        xr_coro_select_block(&f.isolate_storage, &waiter, channels, 2, NULL, 2);
    ASSERT_EQ_INT((int) blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.timer_token.state), XR_TIMER_WAIT_REGISTERED);
    ASSERT_EQ_INT(waiter_ext.wait.timer_token.wait_reason,
                  XR_CORO_WAIT_SELECT >> XR_CORO_WAIT_SHIFT);
    ASSERT_TRUE(atomic_load(&waiter_ext.timer_active));

    ASSERT_EQ_PTR(xr_worker_wake_select(&f.worker, ch), &waiter);

    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.timer_token.state), XR_TIMER_WAIT_CANCELLED);
    ASSERT_FALSE(atomic_load(&waiter_ext.timer_active));
    ASSERT_TRUE(atomic_load(&waiter_ext.select_storage.wait.triggered));
    ASSERT_EQ_INT(atomic_load(&waiter_ext.select_storage.wait.selected_index), 0);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_READY));

    xr_coro_clear_select_wait(&waiter);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.timer_token.state), XR_TIMER_WAIT_IDLE);

    xr_channel_destroy(timer_ch);
    xr_sysheap_free_shared(timer_ch, sizeof(XrChannel) + sizeof(XrValue));
    xr_channel_destroy(ch);
    xr_sysheap_free_shared(ch, sizeof(XrChannel));
    close_fixture_cleanup(&f);
}

TEST(backend_resume_finishes_sleep_timer_token) {
    XrCoroutine sleeper;
    XrCoroExt sleeper_ext;
    memset(&sleeper, 0, sizeof(sleeper));
    memset(&sleeper_ext, 0, sizeof(sleeper_ext));
    sleeper.ext = &sleeper_ext;
    atomic_store(&sleeper.flags, XR_CORO_FLG_READY | XR_CORO_WAIT_SLEEP);
    atomic_store(&sleeper.resume_status, XR_RESUME_TIMEOUT);

    xr_timer_wait_token_prepare(&sleeper_ext.wait.timer_token, 0,
                                XR_CORO_WAIT_SLEEP >> XR_CORO_WAIT_SHIFT, 123);
    xr_timer_wait_token_commit(&sleeper_ext.wait.timer_token);
    xr_timer_wait_token_fire(&sleeper_ext.wait.timer_token);
    ASSERT_EQ_INT(atomic_load(&sleeper_ext.wait.timer_token.state), XR_TIMER_WAIT_FIRED);

    xr_coro_finish_backend_resume_tokens(&sleeper, xr_coro_resume_load(&sleeper));

    ASSERT_EQ_INT(atomic_load(&sleeper_ext.wait.timer_token.state), XR_TIMER_WAIT_IDLE);
}

TEST(multi_await_token_clears_pending_waiters_on_await_any_ready) {
    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 601;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);

    XrTask pending;
    XrTask done;
    init_stack_task(&pending, XR_TASK_ACTIVE, xr_null(), xr_null());
    init_stack_task(&done, XR_TASK_COMPLETED, xr_int(77), xr_null());

    XrArray tasks;
    ASSERT_TRUE(init_task_array(&tasks, 2));
    xr_array_push(&tasks, xr_value_from_task(&pending));
    xr_array_push(&tasks, xr_value_from_task(&done));

    XrCoroBlockResult result = xr_coro_await_any_task(&waiter, &tasks, false);
    ASSERT_EQ_INT((int) result.kind, (int) XR_CORO_BLOCK_READY);
    ASSERT_EQ_INT((int) XR_TO_INT(result.value), 77);
    ASSERT_NULL(pending.await_waiters);
    ASSERT_NULL(
        atomic_load_explicit((_Atomic(XrCoroutine *) *) &pending.waiter, memory_order_acquire));
    ASSERT_EQ_INT(atomic_load_explicit((_Atomic int *) &pending.waiter_index, memory_order_acquire),
                  -1);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.multi_await_token.state), XR_MULTI_AWAIT_WAIT_IDLE);
    ASSERT_NULL(atomic_load(&waiter_ext.wait.multi_await_token.tasks));
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.wait_count), 0);

    destroy_task_array(&tasks);
}

TEST(multi_await_token_cancels_registered_task_waiters) {
    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    memset(&waiter, 0, sizeof(waiter));
    memset(&waiter_ext, 0, sizeof(waiter_ext));
    waiter.id = 602;
    waiter.ext = &waiter_ext;
    atomic_store(&waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&waiter.resume_status, XR_RESUME_OK);

    XrTask first;
    XrTask second;
    init_stack_task(&first, XR_TASK_ACTIVE, xr_null(), xr_null());
    init_stack_task(&second, XR_TASK_ACTIVE, xr_null(), xr_null());

    XrArray tasks;
    ASSERT_TRUE(init_task_array(&tasks, 2));
    xr_array_push(&tasks, xr_value_from_task(&first));
    xr_array_push(&tasks, xr_value_from_task(&second));

    XrCoroBlockResult result = xr_coro_await_all_tasks(&waiter, &tasks);
    ASSERT_EQ_INT((int) result.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.multi_await_token.state),
                  XR_MULTI_AWAIT_WAIT_REGISTERED);
    ASSERT_EQ_PTR(first.await_waiters, &waiter_ext.wait.multi_await_token.nodes[0]);
    ASSERT_EQ_PTR(second.await_waiters, &waiter_ext.wait.multi_await_token.nodes[1]);
    ASSERT_EQ_PTR(waiter_ext.wait.multi_await_token.nodes[0].waiter, &waiter);
    ASSERT_EQ_PTR(waiter_ext.wait.multi_await_token.nodes[1].waiter, &waiter);

    xr_coro_cancel(&waiter);

    ASSERT_NULL(first.await_waiters);
    ASSERT_NULL(second.await_waiters);
    ASSERT_NULL(
        atomic_load_explicit((_Atomic(XrCoroutine *) *) &first.waiter, memory_order_acquire));
    ASSERT_NULL(
        atomic_load_explicit((_Atomic(XrCoroutine *) *) &second.waiter, memory_order_acquire));
    ASSERT_EQ_INT(atomic_load_explicit((_Atomic int *) &first.waiter_index, memory_order_acquire),
                  -1);
    ASSERT_EQ_INT(atomic_load_explicit((_Atomic int *) &second.waiter_index, memory_order_acquire),
                  -1);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.multi_await_token.state),
                  XR_MULTI_AWAIT_WAIT_CANCELLED);
    ASSERT_NULL(atomic_load(&waiter_ext.wait.multi_await_token.tasks));
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.wait_count), 0);

    destroy_task_array(&tasks);
}

TEST(multi_await_node_list_wakes_overlapping_any_waiters) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine first_waiter;
    XrCoroutine second_waiter;
    XrCoroExt first_ext;
    XrCoroExt second_ext;
    memset(&first_waiter, 0, sizeof(first_waiter));
    memset(&second_waiter, 0, sizeof(second_waiter));
    memset(&first_ext, 0, sizeof(first_ext));
    memset(&second_ext, 0, sizeof(second_ext));
    first_waiter.id = 603;
    second_waiter.id = 604;
    first_waiter.isolate = &f.isolate_storage;
    second_waiter.isolate = &f.isolate_storage;
    first_waiter.ext = &first_ext;
    second_waiter.ext = &second_ext;
    atomic_store(&first_waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&second_waiter.flags, XR_CORO_FLG_RUNNING);
    atomic_store(&first_waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&second_waiter.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&first_waiter.resume_status, XR_RESUME_OK);
    atomic_store(&second_waiter.resume_status, XR_RESUME_OK);
    atomic_store(&first_waiter.affinity_p, 0);
    atomic_store(&second_waiter.affinity_p, 0);

    XrTask shared;
    init_stack_task(&shared, XR_TASK_ACTIVE, xr_null(), xr_null());

    XrArray tasks;
    ASSERT_TRUE(init_task_array(&tasks, 1));
    xr_array_push(&tasks, xr_value_from_task(&shared));

    XrCoroBlockResult first_blocked = xr_coro_await_any_task(&first_waiter, &tasks, false);
    XrCoroBlockResult second_blocked = xr_coro_await_any_task(&second_waiter, &tasks, false);
    ASSERT_EQ_INT((int) first_blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_EQ_INT((int) second_blocked.kind, (int) XR_CORO_BLOCK_BLOCKED);
    ASSERT_NOT_NULL(shared.await_waiters);
    ASSERT_NOT_NULL(shared.await_waiters->next);

    atomic_store(&first_waiter.flags, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_AWAIT_ANY);
    atomic_store(&second_waiter.flags, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_AWAIT_ANY);
    atomic_store(&first_waiter.coro_state, XR_CORO_STATE_BLOCKED);
    atomic_store(&second_waiter.coro_state, XR_CORO_STATE_BLOCKED);
    shared.result = xr_int(88);
    atomic_store(&shared.state, XR_TASK_COMPLETED);
    xr_task_wake_waiter(&f.isolate_storage, &shared);

    ASSERT_NULL(shared.await_waiters);
    ASSERT_EQ_INT(atomic_load(&first_ext.wait.multi_await_token.state),
                  XR_MULTI_AWAIT_WAIT_RESOLVED);
    ASSERT_EQ_INT(atomic_load(&second_ext.wait.multi_await_token.state),
                  XR_MULTI_AWAIT_WAIT_RESOLVED);
    ASSERT_EQ_INT(XR_TO_INT(first_waiter.result), 88);
    ASSERT_EQ_INT(XR_TO_INT(second_waiter.result), 88);
    ASSERT_TRUE(xr_coro_flags_has(&first_waiter, XR_CORO_FLG_READY));
    ASSERT_TRUE(xr_coro_flags_has(&second_waiter, XR_CORO_FLG_READY));

    XrCoroutine *popped_a = xr_worker_pop(&f.worker);
    XrCoroutine *popped_b = xr_worker_pop(&f.worker);
    ASSERT_TRUE((popped_a == &first_waiter && popped_b == &second_waiter) ||
                (popped_a == &second_waiter && popped_b == &first_waiter));

    xr_task_finish_await_waiters(&first_waiter);
    xr_task_finish_await_waiters(&second_waiter);
    destroy_task_array(&tasks);
    close_fixture_cleanup(&f);
}

TEST(io_wait_token_tracks_netpoll_ready_and_cancel) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    init_blocked_io_coro(&waiter, &waiter_ext, 701, &f.isolate_storage, 44, XR_POLL_READ, 1000);

    XrPollDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.fd = 44;
    pd.netpoll = &f.runtime.netpoll;
    atomic_store(&f.runtime.netpoll.waiters, 1);
    atomic_store(&pd.rg, (uintptr_t) &waiter);

    ASSERT_EQ_PTR(xr_netpoll_unblock(&pd, XR_POLL_READ, true), &waiter);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.io_token.state), XR_IO_WAIT_READY);
    ASSERT_EQ_INT(atomic_load(&f.runtime.netpoll.waiters), 0);
    ASSERT_EQ_INT((int) atomic_load(&pd.rg), XR_PD_NIL);

    init_blocked_io_coro(&waiter, &waiter_ext, 701, &f.isolate_storage, 44, XR_POLL_READ, 1000);
    atomic_store(&f.runtime.netpoll.waiters, 1);
    atomic_store(&pd.rg, (uintptr_t) &waiter);

    ASSERT_EQ_PTR(xr_netpoll_unblock(&pd, XR_POLL_READ, false), &waiter);
    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.io_token.state), XR_IO_WAIT_CANCELLED);
    ASSERT_EQ_INT(atomic_load(&f.runtime.netpoll.waiters), 0);
    ASSERT_EQ_INT((int) atomic_load(&pd.rg), XR_PD_NIL);

    close_fixture_cleanup(&f);
}

TEST(io_wait_token_tracks_netpoll_deadline_timeout) {
    CloseFixture f;
    ASSERT_TRUE(close_fixture_init(&f));

    XrCoroutine waiter;
    XrCoroExt waiter_ext;
    init_blocked_io_coro(&waiter, &waiter_ext, 702, &f.isolate_storage, 45, XR_POLL_READ, 25);

    XrPollDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.fd = 45;
    pd.netpoll = &f.runtime.netpoll;
    pd.rseq_saved = 9;
    atomic_store(&pd.rseq, 9);
    atomic_store(&pd.rg, (uintptr_t) &waiter);
    atomic_store(&f.runtime.netpoll.waiters, 1);

    xr_netpoll_deadline_impl(&pd, 9, true);

    ASSERT_EQ_INT(atomic_load(&waiter_ext.wait.io_token.state), XR_IO_WAIT_TIMED_OUT);
    ASSERT_EQ_INT(atomic_load(&f.runtime.netpoll.waiters), 0);
    ASSERT_EQ_INT(xr_coro_resume_load(&waiter), XR_RESUME_TIMEOUT);
    ASSERT_TRUE(xr_coro_flags_has(&waiter, XR_CORO_FLG_READY));
    ASSERT_FALSE(xr_coro_flags_has(&waiter, XR_CORO_FLG_BLOCKED));

    worker_drain_inbox(&f.worker);
    ASSERT_EQ_PTR(xr_worker_pop(&f.worker), &waiter);

    close_fixture_cleanup(&f);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Channel Close");
RUN_TEST(channel_close_wakes_select_waiter_without_caller_fanout);
RUN_TEST(channel_close_batches_select_waiter_wakes);
RUN_TEST(select_block_rechecks_already_closed_channel);
RUN_TEST(select_waiter_cancel_unlinks_worker_buckets);
RUN_TEST(channel_ready_wake_dispatches_single_remote_worker);
RUN_TEST(channel_wake_command_batches_ready_waiters);
RUN_TEST(channel_direction_masks_refresh_after_partial_wake);
RUN_TEST(channel_shape_op_metrics_track_logical_and_worker_kinds);
RUN_TEST(channel_wait_token_tracks_block_wake_and_resume);
RUN_TEST(channel_timed_wait_cancels_timer_on_channel_wake);
RUN_TEST(channel_notify_send_without_current_worker_routes_to_inbox);
RUN_TEST(channel_waiter_cancel_unlinks_channel_and_worker_queues);
RUN_TEST(channel_waiter_cancel_routes_foreign_owner_detach);
RUN_TEST(select_waiter_cancel_routes_foreign_owner_detach);
RUN_TEST(await_wait_token_tracks_register_resolve_and_resume);
RUN_TEST(single_task_await_wakes_multiple_waiters);
RUN_TEST(single_task_await_legacy_and_node_wake_claim_once);
RUN_TEST(single_task_await_timeout_unlinks_waiter_node);
RUN_TEST(timer_wait_token_tracks_channel_timeout_cancel_on_wake);
RUN_TEST(timer_wait_token_cancels_select_timer_on_channel_wake);
RUN_TEST(backend_resume_finishes_sleep_timer_token);
RUN_TEST(multi_await_token_clears_pending_waiters_on_await_any_ready);
RUN_TEST(multi_await_token_cancels_registered_task_waiters);
RUN_TEST(multi_await_node_list_wakes_overlapping_any_waiters);
RUN_TEST(io_wait_token_tracks_netpoll_ready_and_cancel);
RUN_TEST(io_wait_token_tracks_netpoll_deadline_timeout);

TEST_MAIN_END()
