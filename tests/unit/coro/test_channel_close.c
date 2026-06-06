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

static void init_blocked_channel_coro(XrCoroutine *coro, XrCoroExt *ext, int id,
                                      XrayIsolate *isolate, XrChannel *ch, bool wait_send) {
    memset(coro, 0, sizeof(*coro));
    memset(ext, 0, sizeof(*ext));
    coro->id = id;
    coro->isolate = isolate;
    coro->ext = ext;
    uint32_t wait_flag = wait_send ? XR_CORO_WAIT_CHANNEL_SEND : XR_CORO_WAIT_CHANNEL_RECV;
    atomic_store(&coro->flags, XR_CORO_FLG_BLOCKED | wait_flag | XR_CORO_PRIO_NORMAL);
    atomic_store(&coro->coro_state, XR_CORO_STATE_BLOCKED);
    atomic_store(&coro->resume_status, XR_RESUME_OK);
    atomic_store(&coro->affinity_p, 0);
    xr_channel_wait_token_prepare(&ext->chan_wait_token, ch, wait_send);
    xr_channel_wait_token_commit(&ext->chan_wait_token);
    atomic_store_explicit(&ext->wait_channel, ch, memory_order_release);
    ext->wait_send = wait_send;
    ext->wait_bucket_owner = -1;
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
    atomic_store(&coro.flags, XR_CORO_FLG_RUNNING | XR_CORO_PRIO_NORMAL);
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

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(1), &producer), (int) XR_CHAN_OK);
    XrValue out = xr_null();
    ASSERT_EQ_INT((int) xr_channel_recv(ch, &out, &consumer), (int) XR_CHAN_OK);
    ASSERT_EQ_INT((int) XR_TO_INT(out), 1);

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(2), &producer), (int) XR_CHAN_OK);
    ASSERT_EQ_INT((int) xr_channel_recv(ch, &out, &consumer), (int) XR_CHAN_OK);
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
    atomic_store(&sender.flags, XR_CORO_FLG_RUNNING | XR_CORO_PRIO_NORMAL);
    atomic_store(&sender.coro_state, XR_CORO_STATE_RUNNING);
    atomic_store(&sender.resume_status, XR_RESUME_OK);
    atomic_store(&sender.affinity_p, 0);

    ASSERT_EQ_INT((int) xr_channel_send(ch, xr_int(7), &sender), (int) XR_CHAN_BLOCK);
    ASSERT_EQ_INT(atomic_load(&sender_ext.chan_wait_token.state), XR_CHAN_WAIT_REGISTERED);
    ASSERT_EQ_PTR(atomic_load(&sender_ext.chan_wait_token.channel), ch);
    ASSERT_TRUE(sender_ext.chan_wait_token.is_send);

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

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Channel Close");
RUN_TEST(channel_close_wakes_select_waiter_without_caller_fanout);
RUN_TEST(select_block_rechecks_already_closed_channel);
RUN_TEST(channel_ready_wake_dispatches_single_remote_worker);
RUN_TEST(channel_direction_masks_refresh_after_partial_wake);
RUN_TEST(channel_shape_op_metrics_track_logical_and_worker_kinds);
RUN_TEST(channel_wait_token_tracks_block_wake_and_resume);

TEST_MAIN_END()
