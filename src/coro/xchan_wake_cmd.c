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

/* ========== Queue Init / Destroy ========== */

void xr_chan_wake_queue_init(XrChanWakeCmdQueue *q) {
    XR_DCHECK(q != NULL, "chan_wake_queue_init: NULL queue");
    q->stub.next = NULL;
    q->stub.channel = NULL;
    q->stub.wake_sender = false;
    q->stub.is_close = false;
    atomic_store_explicit(&q->head, &q->stub, memory_order_relaxed);
    atomic_store_explicit(&q->tail, &q->stub, memory_order_relaxed);
}

void xr_chan_wake_queue_destroy(XrChanWakeCmdQueue *q) {
    if (!q) return;

    // Drain residual nodes (head may be stub or a real node)
    XrChanWakeCmd *node = atomic_load(&q->head);
    if (node == &q->stub) {
        node = q->stub.next;
    }
    while (node) {
        XrChanWakeCmd *next = node->next;
        xr_free(node);
        node = next;
    }
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
    atomic_store_explicit((_Atomic(XrChanWakeCmd *)*)&prev->next, cmd, memory_order_release);
}

/* ========== Dispatch (called from any thread for remote worker) ========== */

void xr_worker_dispatch_chan_wake(XrRuntime *runtime, int target_id,
                                  void *channel, bool wake_sender,
                                  bool is_close) {
    XR_DCHECK(runtime != NULL, "dispatch_chan_wake: NULL runtime");
    XR_DCHECK(channel != NULL, "dispatch_chan_wake: NULL channel");
    XR_DCHECK(target_id >= 0 && target_id < runtime->worker_count,
              "dispatch_chan_wake: target_id out of range");

    XrChanWakeCmd *cmd = (XrChanWakeCmd *)xr_malloc(sizeof(XrChanWakeCmd));
    if (!cmd) return;  // OOM: waiter stays blocked until timeout / next event

    cmd->channel = channel;
    cmd->wake_sender = wake_sender;
    cmd->is_close = is_close;

    XrWorker *target = &runtime->workers[target_id];
    chan_wake_queue_push(&target->p.chan_wake_queue, cmd);

    // Dekker fence: ensure push is visible before reading target state.
    // Pairs with seq_cst store of M_PARKING in worker_park.
    atomic_thread_fence(memory_order_seq_cst);

    // Wake target worker if parked
    if (atomic_load(&target->m->state) == M_PARKING) {
        worker_unpark(target);
    }
}

/* ========== Drain (called by owner worker in scheduling loop) ========== */

void xr_worker_drain_chan_wake_queue(XrWorker *worker) {
    XR_DCHECK(worker != NULL, "drain_chan_wake_queue: NULL worker");
    XrChanWakeCmdQueue *q = &worker->p.chan_wake_queue;

    // Vyukov MPSC consumer: advance head past stub/old nodes
    while (1) {
        XrChanWakeCmd *head = atomic_load_explicit(&q->head, memory_order_acquire);
        XrChanWakeCmd *next = atomic_load_explicit(
            (_Atomic(XrChanWakeCmd *)*)&head->next, memory_order_acquire);

        if (next == NULL) {
            // Empty or in-progress enqueue
            break;
        }

        // Extract command data from 'next' (the real payload node)
        void *channel = next->channel;
        bool wake_sender = next->wake_sender;
        bool is_close = next->is_close;

        // Advance head (dequeue)
        atomic_store_explicit(&q->head, next, memory_order_release);

        // Free old head (stub is embedded, don't free it)
        if (head != &q->stub) {
            xr_free(head);
        }

        // Execute local wake on owner's own thread (safe: owner-private access)
        if (is_close) {
            // Close path: wake ALL normal + select waiters on this channel
            xr_worker_wake_all(worker, channel);
            while (xr_worker_wake_select(worker, channel)) {
                // Keep waking until no more select waiters
            }
        } else {
            // Normal send/recv path: wake ONE normal waiter or select waiter
            XrCoroutine *coro = xr_worker_wake_one(worker, channel, wake_sender);
            if (!coro) {
                xr_worker_wake_select(worker, channel);
            }
        }
    }
}
