/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchan_wake_cmd.c - Channel wake command MPSC queue
 *
 * KEY CONCEPT:
 *   When a channel send/recv/close succeeds, remote workers that may have
 *   blocked waiters receive a lightweight command via this MPSC queue.
 *   The owning worker drains the queue in its scheduling loop and performs
 *   local wake_one / wake_select / wake_all on its own thread, preserving
 *   the owner-private invariant for blocked buckets and run queues.
 *
 *   Queue uses Vyukov MPSC (same pattern as XrTimerCancelQueue):
 *   - Producers CAS-push to tail (lock-free, multi-producer safe)
 *   - Consumer advances head (single-consumer, the owner worker)
 *   - Stub sentinel avoids empty-queue edge cases
 */

#include "xworker_internal.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"

/* ========== Command Pool ==========
 *
 * The MPSC queue keeps the last drained payload node as its sentinel.  A pool
 * command is therefore returned one drain later, or immediately when the
 * consumer proves the queue is empty and swaps the embedded stub back in.
 */

static void chan_wake_cmd_reset(XrChanWakeCmd *cmd) {
    if (!cmd)
        return;
    cmd->channel = NULL;
    cmd->wake_sender = false;
    cmd->is_close = false;
    cmd->next = NULL;
}

static void chan_wake_pool_init(XrChanWakeCmdPool *pool) {
    XR_DCHECK(pool != NULL, "chan_wake_pool_init: NULL pool");
    xr_mutex_init(&pool->lock);
    pool->free_list = NULL;
    pool->blocks = NULL;
    pool->free_count = 0;
    pool->block_count = 0;
}

static void chan_wake_pool_destroy(XrChanWakeCmdPool *pool) {
    if (!pool)
        return;
    XrChanWakeCmdBlock *block = pool->blocks;
    while (block) {
        XrChanWakeCmdBlock *next = block->next;
        xr_free(block);
        block = next;
    }
    pool->free_list = NULL;
    pool->blocks = NULL;
    pool->free_count = 0;
    pool->block_count = 0;
    xr_mutex_destroy(&pool->lock);
}

static XrChanWakeCmd *chan_wake_pool_take_locked(XrChanWakeCmdPool *pool) {
    XrChanWakeCmd *cmd = pool->free_list;
    if (!cmd)
        return NULL;
    pool->free_list = cmd->next;
    pool->free_count--;
    cmd->next = NULL;
    return cmd;
}

static XrChanWakeCmd *chan_wake_pool_grow_locked(XrRuntime *runtime, XrChanWakeCmdPool *pool) {
    XrChanWakeCmdBlock *block = (XrChanWakeCmdBlock *) xr_malloc(sizeof(XrChanWakeCmdBlock));
    if (!block)
        return NULL;

    block->next = pool->blocks;
    pool->blocks = block;
    pool->block_count++;

    for (int i = 1; i < XR_CHAN_WAKE_CMD_POOL_BATCH; i++) {
        XrChanWakeCmd *cmd = &block->cmds[i];
        chan_wake_cmd_reset(cmd);
        cmd->from_heap = false;
        cmd->is_emergency = false;
        cmd->next = pool->free_list;
        pool->free_list = cmd;
        pool->free_count++;
    }

    xr_sched_metric_add(runtime, &runtime->sched_stats.chan_wake_cmd_alloc_count,
                        XR_CHAN_WAKE_CMD_POOL_BATCH);
    XrChanWakeCmd *cmd = &block->cmds[0];
    chan_wake_cmd_reset(cmd);
    cmd->from_heap = false;
    cmd->is_emergency = false;
    return cmd;
}

static XrChanWakeCmd *chan_wake_pool_take(XrRuntime *runtime, XrChanWakeCmdQueue *q) {
    XrChanWakeCmdPool *pool = &q->pool;
    xr_mutex_lock(&pool->lock);
    XrChanWakeCmd *cmd = chan_wake_pool_take_locked(pool);
    if (!cmd) {
        cmd = chan_wake_pool_grow_locked(runtime, pool);
    }
    xr_mutex_unlock(&pool->lock);
    return cmd;
}

static void chan_wake_cmd_release(XrRuntime *runtime, XrChanWakeCmdQueue *q, XrChanWakeCmd *cmd) {
    if (!cmd || cmd == &q->stub)
        return;
    bool from_heap = cmd->from_heap;
    bool is_emergency = cmd->is_emergency;
    chan_wake_cmd_reset(cmd);
    cmd->from_heap = from_heap;
    cmd->is_emergency = is_emergency;

    if (is_emergency) {
        atomic_store_explicit(&q->emergency_in_use, false, memory_order_release);
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_free_count);
        return;
    }
    if (from_heap) {
        xr_free(cmd);
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_free_count);
        return;
    }

    XrChanWakeCmdPool *pool = &q->pool;
    xr_mutex_lock(&pool->lock);
    cmd->next = pool->free_list;
    pool->free_list = cmd;
    pool->free_count++;
    xr_mutex_unlock(&pool->lock);
    xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_free_count);
}

static XrChanWakeCmd *chan_wake_cmd_alloc(XrRuntime *runtime, XrWorker *target) {
    XrChanWakeCmdQueue *q = &target->p.chan_wake_queue;
    while (!runtime || atomic_load_explicit(&runtime->running, memory_order_relaxed)) {
        XrChanWakeCmd *cmd = chan_wake_pool_take(runtime, q);
        if (cmd)
            return cmd;

        cmd = (XrChanWakeCmd *) xr_malloc(sizeof(XrChanWakeCmd));
        if (cmd) {
            chan_wake_cmd_reset(cmd);
            cmd->from_heap = true;
            cmd->is_emergency = false;
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_alloc_count);
            return cmd;
        }

        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(&q->emergency_in_use, &expected, true,
                                                    memory_order_acq_rel, memory_order_relaxed)) {
            cmd = &q->emergency_cmd;
            chan_wake_cmd_reset(cmd);
            cmd->from_heap = false;
            cmd->is_emergency = true;
            return cmd;
        }

        worker_unpark(target);
        xr_thread_yield();
    }
    return NULL;
}

/* ========== Queue Init / Destroy ========== */

void xr_chan_wake_queue_init(XrChanWakeCmdQueue *q) {
    XR_DCHECK(q != NULL, "chan_wake_queue_init: NULL queue");
    q->stub.next = NULL;
    q->stub.channel = NULL;
    q->stub.wake_sender = false;
    q->stub.is_close = false;
    q->stub.from_heap = false;
    q->stub.is_emergency = false;
    chan_wake_pool_init(&q->pool);
    chan_wake_cmd_reset(&q->emergency_cmd);
    q->emergency_cmd.from_heap = false;
    q->emergency_cmd.is_emergency = true;
    atomic_store_explicit(&q->emergency_in_use, false, memory_order_relaxed);
    atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
    atomic_store_explicit(&q->tail, &q->stub, memory_order_relaxed);
}

void xr_chan_wake_queue_destroy(XrChanWakeCmdQueue *q) {
    if (!q)
        return;

    // Drain residual nodes (head may be stub or a real node)
    XrChanWakeCmd *node = atomic_load(&q->head);
    if (node == &q->stub) {
        node = q->stub.next;
    }
    while (node) {
        XrChanWakeCmd *next = node->next;
        if (node->from_heap) {
            xr_free(node);
        } else if (node->is_emergency) {
            atomic_store_explicit(&q->emergency_in_use, false, memory_order_relaxed);
        }
        node = next;
    }
    chan_wake_pool_destroy(&q->pool);
    // Reset to clean state (stub is embedded, not freed)
    q->stub.next = NULL;
    atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
    atomic_store_explicit(&q->tail, &q->stub, memory_order_relaxed);
}

/* ========== MPSC Enqueue (producer side, any thread) ========== */

static void chan_wake_queue_push(XrChanWakeCmdQueue *q, XrChanWakeCmd *cmd) {
    XR_DCHECK(q != NULL, "chan_wake_queue_push: NULL queue");
    XR_DCHECK(cmd != NULL, "chan_wake_queue_push: NULL cmd");
    cmd->next = NULL;

    // Vyukov MPSC enqueue: swap tail, link previous tail to new node
    XrChanWakeCmd *prev = atomic_exchange_explicit(&q->tail, cmd, memory_order_acq_rel);
    atomic_store_explicit((_Atomic(XrChanWakeCmd *) *) &prev->next, cmd, memory_order_release);
}

/* ========== Dispatch (called from any thread for remote worker) ========== */

void xr_worker_dispatch_chan_wake(XrRuntime *runtime, int target_id, void *channel,
                                  bool wake_sender, bool is_close) {
    XR_DCHECK(runtime != NULL, "dispatch_chan_wake: NULL runtime");
    XR_DCHECK(channel != NULL, "dispatch_chan_wake: NULL channel");
    XR_DCHECK(target_id >= 0 && target_id < runtime->worker_count,
              "dispatch_chan_wake: target_id out of range");

    XrWorker *target = &runtime->workers[target_id];
    XrChanWakeCmd *cmd = chan_wake_cmd_alloc(runtime, target);
    if (!cmd)
        return;

    cmd->channel = channel;
    cmd->wake_sender = wake_sender;
    cmd->is_close = is_close;

    chan_wake_queue_push(&target->p.chan_wake_queue, cmd);
    if (runtime) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_dispatch_count);
    }

    // Dekker fence: ensure push is visible before reading target state.
    // Pairs with seq_cst store of M_PARKING in worker_park.
    atomic_thread_fence(memory_order_seq_cst);

    // Wake target worker if parked and currently bound to an M.
    XrMachine *target_m = atomic_load_explicit(&target->p.current_m, memory_order_acquire);
    if (target_m && atomic_load_explicit(&target_m->state, memory_order_acquire) == M_PARKING) {
        worker_unpark(target);
    }
}

/* ========== Drain (called by owner worker in scheduling loop) ========== */

static bool chan_wake_cmd_matches(const XrChanWakeCmd *cmd, void *channel, bool wake_sender,
                                  bool is_close) {
    if (!cmd || cmd->channel != channel || cmd->is_close != is_close)
        return false;
    return is_close || cmd->wake_sender == wake_sender;
}

static bool chan_wake_queue_reclaim_empty_head(XrRuntime *runtime, XrChanWakeCmdQueue *q,
                                               XrChanWakeCmd *head) {
    if (!head || head == &q->stub)
        return false;
    XrChanWakeCmd *expected = head;
    q->stub.next = NULL;
    q->stub.channel = NULL;
    q->stub.wake_sender = false;
    q->stub.is_close = false;
    q->stub.from_heap = false;
    q->stub.is_emergency = false;
    if (!atomic_compare_exchange_strong_explicit(&q->tail, &expected, &q->stub,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }
    atomic_store_explicit(&q->head, &q->stub, memory_order_release);
    chan_wake_cmd_release(runtime, q, head);
    return true;
}

static int chan_wake_queue_take_coalesced(XrRuntime *runtime, XrChanWakeCmdQueue *q,
                                          void **channel_out, bool *wake_sender_out,
                                          bool *is_close_out) {
    XrChanWakeCmd *head = atomic_load_explicit(&q->head, memory_order_acquire);
    XrChanWakeCmd *next =
        atomic_load_explicit((_Atomic(XrChanWakeCmd *) *) &head->next, memory_order_acquire);

    if (next == NULL) {
        (void) chan_wake_queue_reclaim_empty_head(runtime, q, head);
        return 0;
    }

    void *channel = next->channel;
    bool wake_sender = next->wake_sender;
    bool is_close = next->is_close;
    int count = 1;

    atomic_store_explicit(&q->head, next, memory_order_release);
    xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_drain_count);
    chan_wake_cmd_release(runtime, q, head);

    XrChanWakeCmd *sentinel = next;
    while (1) {
        XrChanWakeCmd *after = atomic_load_explicit((_Atomic(XrChanWakeCmd *) *) &sentinel->next,
                                                    memory_order_acquire);
        if (!chan_wake_cmd_matches(after, channel, wake_sender, is_close))
            break;
        atomic_store_explicit(&q->head, after, memory_order_release);
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_drain_count);
        chan_wake_cmd_release(runtime, q, sentinel);
        sentinel = after;
        count++;
    }

    if (count > 1) {
        xr_sched_metric_add(runtime, &runtime->sched_stats.chan_wake_cmd_coalesce_count,
                            (uint64_t) (count - 1));
    }

    *channel_out = channel;
    *wake_sender_out = wake_sender;
    *is_close_out = is_close;
    return count;
}

static bool worker_channel_has_waiter(XrWorker *worker, void *channel) {
    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    return bucket && (bucket->send_head || bucket->recv_head || bucket->select_head);
}

static void worker_forward_chan_wakes(void *channel, bool wake_sender, int count) {
    if (!channel || count <= 0)
        return;
    XrChannel *ch = (XrChannel *) channel;
    for (int i = 0; i < count; i++) {
        (void) xr_runtime_wake_channel(ch->isolate, channel, wake_sender);
    }
}

static void chan_ready_append(XrCoroutine **first, XrCoroutine **last, XrCoroutine *coro) {
    if (!coro)
        return;
    coro->sched_link = NULL;
    if (*last) {
        (*last)->sched_link = coro;
    } else {
        *first = coro;
    }
    *last = coro;
}

static void chan_ready_flush(XrWorker *worker, XrCoroutine **first, XrCoroutine **last) {
    if (!first || !last || !*first)
        return;
    (void) xr_worker_push_lifo_batch(worker, *first);
    *first = NULL;
    *last = NULL;
}

static void worker_execute_chan_wake_batch(XrWorker *worker, void *channel, bool wake_sender,
                                           bool is_close, int count) {
    XrRuntime *runtime = worker->p.runtime;
    if (is_close) {
        if (!worker_channel_has_waiter(worker, channel)) {
            worker_clear_channel_waiter_mask(worker, channel);
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_stale_count);
            return;
        }
        xr_worker_wake_all(worker, channel);
        (void) xr_worker_wake_select_all_with_status(worker, channel, XR_RESUME_CHANNEL_CLOSED);
        return;
    }

    XrCoroutine *ready_first = NULL;
    XrCoroutine *ready_last = NULL;
    for (int i = 0; i < count; i++) {
        XrCoroutine *ready = NULL;
        bool consumed = xr_worker_wake_one_detached(worker, channel, wake_sender, &ready);
        if (consumed) {
            chan_ready_append(&ready_first, &ready_last, ready);
            continue;
        }

        chan_ready_flush(worker, &ready_first, &ready_last);
        XrCoroutine *coro = xr_worker_wake_select(worker, channel);
        if (!coro) {
            worker_clear_channel_waiter_mask(worker, channel);
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_wake_cmd_stale_count);
            xr_sched_metric_add(runtime, &runtime->sched_stats.chan_wake_cmd_forward_count,
                                (uint64_t) (count - i));
            worker_forward_chan_wakes(channel, wake_sender, count - i);
            break;
        }
    }
    chan_ready_flush(worker, &ready_first, &ready_last);
}

void xr_worker_drain_chan_wake_queue(XrWorker *worker) {
    XR_DCHECK(worker != NULL, "drain_chan_wake_queue: NULL worker");
    XrChanWakeCmdQueue *q = &worker->p.chan_wake_queue;
    XrRuntime *runtime = worker->p.runtime;

    while (1) {
        void *channel = NULL;
        bool wake_sender = false;
        bool is_close = false;
        int count = chan_wake_queue_take_coalesced(runtime, q, &channel, &wake_sender, &is_close);
        if (count <= 0)
            break;
        worker_execute_chan_wake_batch(worker, channel, wake_sender, is_close, count);
    }
}
