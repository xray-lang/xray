/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_blocked.c - Per-worker blocked queue for channel/select waiters
 *
 * KEY CONCEPT:
 *   Each worker owns a hash-bucketed blocked queue indexed by channel
 *   pointer, plus a linear doubly-linked list for cross-channel traversal
 *   (used by select wait and sysmon scans). All operations are lock-free
 *   because the queue is owner-private.
 *
 * INVARIANTS:
 *   - A coroutine may appear in at most one bucket's send/recv queue at
 *     a time (matches the channel wait token plus wait_channel/wait_send).
 *   - The linear list (blocked_head/blocked_tail, threaded via prev/next)
 *     contains every currently-blocked coro on this worker.
 *   - xr_worker_block/unblock are called only from the owner worker's
 *     thread. Cross-worker wakes go through MPSC inbox.
 */
#include "xworker_internal.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xhash.h"
#include <string.h>

// ========== Per-Worker Blocked Queue Operations (lock-free) ==========

// Hash function: Channel pointer -> bucket index (uses unified xr_hash_int)
static inline int blocked_bucket_hash(void *channel) {
    return (int) (xr_hash_int((int64_t) (intptr_t) channel) % XR_BLOCKED_BUCKET_SIZE);
}

// Per-Worker version: find or create blocked bucket for Channel (lock-free)
XrBlockedBucket *worker_blocked_bucket_find_or_create(XrWorker *worker, void *channel) {
    int idx = blocked_bucket_hash(channel);
    XrBlockedBucket *bucket = worker->p.blocked_buckets[idx];

    while (bucket) {
        if (bucket->channel == channel)
            return bucket;
        bucket = bucket->next;
    }

    bucket = (XrBlockedBucket *) xr_malloc(sizeof(XrBlockedBucket));
    if (!bucket)
        return NULL;

    memset(bucket, 0, sizeof(XrBlockedBucket));
    bucket->channel = channel;
    bucket->next = worker->p.blocked_buckets[idx];
    worker->p.blocked_buckets[idx] = bucket;

    return bucket;
}

// Per-Worker version: find blocked bucket for Channel (lock-free)
XrBlockedBucket *worker_blocked_bucket_find(XrWorker *worker, void *channel) {
    int idx = blocked_bucket_hash(channel);
    XrBlockedBucket *bucket = worker->p.blocked_buckets[idx];

    while (bucket) {
        if (bucket->channel == channel)
            return bucket;
        bucket = bucket->next;
    }
    return NULL;
}

// True if the bucket holds no waiter at all — eligible for reclamation.
// send_head / recv_head are populated by xr_worker_block; select_head is
// populated by xr_worker_block_select (xworker_sysmon.c).
static inline bool bucket_is_empty(const XrBlockedBucket *b) {
    return b->send_head == NULL && b->recv_head == NULL && b->select_head == NULL;
}

void worker_clear_channel_waiter_mask(XrWorker *worker, void *channel) {
    if (!worker || !channel)
        return;
    XrChannel *ch = (XrChannel *) channel;
    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket || bucket_is_empty(bucket)) {
        xr_channel_clear_waiter_bit(ch, worker->p.id);
        return;
    }

    uint64_t bit = xr_channel_worker_bit(worker->p.id);
    if (bit == 0)
        return;

    atomic_fetch_or_explicit(&ch->waiter_worker_mask, bit, memory_order_release);
    if (bucket->send_head) {
        atomic_fetch_or_explicit(&ch->sender_waiter_worker_mask, bit, memory_order_release);
    } else {
        xr_channel_clear_sender_waiter_bit(ch, worker->p.id);
    }
    if (bucket->recv_head) {
        atomic_fetch_or_explicit(&ch->receiver_waiter_worker_mask, bit, memory_order_release);
    } else {
        xr_channel_clear_receiver_waiter_bit(ch, worker->p.id);
    }
    if (bucket->select_head) {
        atomic_fetch_or_explicit(&ch->select_waiter_worker_mask, bit, memory_order_release);
    } else {
        xr_channel_clear_select_waiter_bit(ch, worker->p.id);
    }
}

// Reclaim an empty bucket: unlink it from the per-worker hash chain and
// free the malloc'd memory. Owner-private, no lock needed.
//
// Prior to this helper the hash chain grew monotonically for every distinct
// channel ever blocked on — a slow leak that could also lengthen lookup time
// after many short-lived channels had been garbage-collected.
void worker_blocked_bucket_reclaim_if_empty(XrWorker *worker, XrBlockedBucket *bucket) {
    if (!bucket || !bucket_is_empty(bucket))
        return;

    int idx = blocked_bucket_hash(bucket->channel);
    XrBlockedBucket **pp = &worker->p.blocked_buckets[idx];
    while (*pp) {
        if (*pp == bucket) {
            *pp = bucket->next;
            worker_clear_channel_waiter_mask(worker, bucket->channel);
            xr_free(bucket);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void bucket_clear_coro_links(XrCoroutine *coro) {
    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = NULL;
    coro->ext->wait_bucket = NULL;
    coro->ext->wait_bucket_owner = -1;
}

static void worker_cancel_coro_timer_wait(XrWorker *worker, XrCoroutine *coro) {
    if (!coro || !coro->ext ||
        !atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        return;
    }
    xr_worker_cancel_timer(worker, coro);
}

static void bucket_append_coro(XrWorker *worker, XrBlockedBucket *bucket, XrCoroutine *coro) {
    XR_DCHECK(worker != NULL, "bucket_append_coro: NULL worker");
    XR_DCHECK(bucket != NULL, "bucket_append_coro: NULL bucket");
    XR_DCHECK(coro != NULL, "bucket_append_coro: NULL coro");
    XR_DCHECK(coro->ext->wait_bucket == NULL, "bucket_append_coro: coro already linked");

    XrCoroutine **head = coro->ext->wait_send ? &bucket->send_head : &bucket->recv_head;
    XrCoroutine **tail = coro->ext->wait_send ? &bucket->send_tail : &bucket->recv_tail;

    coro->ext->wait_link = NULL;
    coro->ext->wait_prev = *tail;
    coro->ext->wait_bucket = bucket;
    coro->ext->wait_bucket_owner = worker->p.id;
    if (*tail) {
        (*tail)->ext->wait_link = coro;
    } else {
        *head = coro;
    }
    *tail = coro;
}

static void bucket_unlink_coro(XrBlockedBucket *bucket, XrCoroutine *coro) {
    XR_DCHECK(bucket != NULL, "bucket_unlink_coro: NULL bucket");
    XR_DCHECK(coro != NULL, "bucket_unlink_coro: NULL coro");

    XrCoroutine **head = coro->ext->wait_send ? &bucket->send_head : &bucket->recv_head;
    XrCoroutine **tail = coro->ext->wait_send ? &bucket->send_tail : &bucket->recv_tail;

    if (coro->ext->wait_prev) {
        coro->ext->wait_prev->ext->wait_link = coro->ext->wait_link;
    } else if (*head == coro) {
        *head = coro->ext->wait_link;
    }

    if (coro->ext->wait_link) {
        coro->ext->wait_link->ext->wait_prev = coro->ext->wait_prev;
    } else if (*tail == coro) {
        *tail = coro->ext->wait_prev;
    }

    bucket_clear_coro_links(coro);
}

static XrCoroutine *bucket_pop_coro(XrBlockedBucket *bucket, bool wake_sender) {
    XR_DCHECK(bucket != NULL, "bucket_pop_coro: NULL bucket");
    XrCoroutine *coro = wake_sender ? bucket->send_head : bucket->recv_head;
    if (coro) {
        bucket_unlink_coro(bucket, coro);
    }
    return coro;
}

static bool worker_blocked_bucket_remove_coro(XrWorker *worker, XrCoroutine *coro) {
    XrBlockedBucket *bucket = coro ? coro->ext->wait_bucket : NULL;
    if (!bucket)
        return false;
    if (coro->ext->wait_bucket_owner != worker->p.id)
        return false;
    void *channel = bucket->channel;
    bucket_unlink_coro(bucket, coro);
    worker_clear_channel_waiter_mask(worker, channel);
    worker_blocked_bucket_reclaim_if_empty(worker, bucket);
    return true;
}

// Per-Worker version: add coroutine to linear blocked queue (lock-free)
void worker_blocked_list_add(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro)
        return;

    coro->prev = worker->p.blocked_tail;
    coro->next = NULL;

    if (worker->p.blocked_tail) {
        worker->p.blocked_tail->next = coro;
    } else {
        worker->p.blocked_head = coro;
    }
    worker->p.blocked_tail = coro;
}

// Per-Worker version: remove coroutine from linear blocked queue (lock-free)
// Returns true if coro was actually in the list and removed
bool worker_blocked_list_remove(XrWorker *worker, XrCoroutine *coro) {
    if (!coro)
        return false;

    // Check if coro is actually in this worker's blocked list
    if (coro->prev == NULL && coro->next == NULL && worker->p.blocked_head != coro) {
        return false;  // Not in list
    }

    if (coro->prev) {
        coro->prev->next = coro->next;
    } else {
        worker->p.blocked_head = coro->next;
    }

    if (coro->next) {
        coro->next->prev = coro->prev;
    } else {
        worker->p.blocked_tail = coro->prev;
    }

    coro->prev = NULL;
    coro->next = NULL;
    return true;
}

// xr_worker_block - Add coroutine to current Worker's blocked queue (lock-free)
void xr_worker_block(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro)
        return;

    // Prevent duplicate add: if coroutine already in blocked queue, return
    if (coro->next != NULL || coro->prev != NULL || worker->p.blocked_head == coro) {
        return;
    }

    if (coro->ext && coro->ext->wait_bucket) {
        if (coro->ext->wait_bucket_owner != worker->p.id)
            return;
        (void) worker_blocked_bucket_remove_coro(worker, coro);
    }

    // Record Worker where coroutine is (for cross-Worker wake)
    atomic_store_explicit(&coro->affinity_p, worker->p.id, memory_order_relaxed);

    // Add to linear queue tail
    coro->prev = worker->p.blocked_tail;
    coro->next = NULL;

    if (worker->p.blocked_tail) {
        worker->p.blocked_tail->next = coro;
    } else {
        worker->p.blocked_head = coro;
    }
    worker->p.blocked_tail = coro;

    // If has Channel, add to hash table using the worker-owned wait links.
    void *wch = atomic_load_explicit(&coro->ext->wait_channel, memory_order_acquire);
    if (wch) {
        XrBlockedBucket *bucket = worker_blocked_bucket_find_or_create(worker, wch);
        if (bucket) {
            bucket_append_coro(worker, bucket, coro);
        }
    }

    worker->p.blocked_count++;
}

// xr_worker_unblock - Remove coroutine from Worker's blocked queue (lock-free)
void xr_worker_unblock(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro)
        return;

    if (worker_blocked_list_remove(worker, coro)) {
        worker->p.blocked_count--;
    }
    (void) worker_blocked_bucket_remove_coro(worker, coro);
}

static XrCoroutine *worker_pop_channel_waiter(XrWorker *worker, void *channel, bool wake_sender,
                                              XrBlockedBucket **bucket_out) {
    if (bucket_out) {
        *bucket_out = NULL;
    }
    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket)
        return NULL;

    XrCoroutine *coro = bucket_pop_coro(bucket, wake_sender);
    if (!coro)
        return NULL;
    worker_clear_channel_waiter_mask(worker, channel);

    if (worker_blocked_list_remove(worker, coro)) {
        worker->p.blocked_count--;
    }
    if (bucket_out) {
        *bucket_out = bucket;
    }
    return coro;
}

static void worker_prepare_channel_waiter_resume(XrWorker *worker, XrCoroutine *coro,
                                                 int resume_status) {
    xr_channel_wait_token_resolve(&coro->ext->chan_wait_token);
    atomic_store_explicit(&coro->ext->wait_channel, NULL, memory_order_relaxed);
    worker_cancel_coro_timer_wait(worker, coro);
    xr_coro_resume_store(coro, resume_status);
}

// xr_worker_wake_one - Wake one coroutine waiting on specified Channel on current Worker
// (lock-free) MUST only be called from the owning worker thread.
XrCoroutine *xr_worker_wake_one(XrWorker *worker, void *channel, bool wake_sender) {
    if (!worker || !channel)
        return NULL;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "wake_one: cross-worker call detected (use chan_wake_queue)");

    XrBlockedBucket *bucket = NULL;
    XrCoroutine *coro = worker_pop_channel_waiter(worker, channel, wake_sender, &bucket);
    if (!coro)
        return NULL;

    // Atomically claim BLOCKED->READY and enqueue to this Worker's LIFO slot
    // for locality. A racing cross-worker waker (channel_wake_coro_ex on the
    // close path) may have already claimed it; only the winner enqueues so the
    // coro is never double-pushed.
    if (xr_coro_claim_wake(coro)) {
        worker_prepare_channel_waiter_resume(worker, coro, XR_RESUME_CHANNEL);
        XrRuntime *runtime = worker->p.runtime;
        if (xr_sched_stats_enabled(runtime)) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_ready_wake_count);
            xr_channel_record_ready_wake_metric(
                runtime, xr_channel_logical_kind_snapshot((XrChannel *) channel), wake_sender);
        }
        xr_worker_push_lifo(worker, coro);
    }

    // Reclaim the bucket if this was the last waiter on the channel.
    worker_blocked_bucket_reclaim_if_empty(worker, bucket);

    return coro;
}

bool xr_worker_wake_one_detached(XrWorker *worker, void *channel, bool wake_sender,
                                 XrCoroutine **ready_out) {
    if (ready_out) {
        *ready_out = NULL;
    }
    if (!worker || !channel)
        return false;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "wake_one_detached: cross-worker call detected (use chan_wake_queue)");

    XrBlockedBucket *bucket = NULL;
    XrCoroutine *coro = worker_pop_channel_waiter(worker, channel, wake_sender, &bucket);
    if (!coro)
        return false;

    if (xr_coro_claim_wake(coro)) {
        worker_prepare_channel_waiter_resume(worker, coro, XR_RESUME_CHANNEL);
        XrRuntime *runtime = worker->p.runtime;
        if (xr_sched_stats_enabled(runtime)) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_ready_wake_count);
            xr_channel_record_ready_wake_metric(
                runtime, xr_channel_logical_kind_snapshot((XrChannel *) channel), wake_sender);
        }
        if (ready_out) {
            *ready_out = coro;
        }
    }
    worker_blocked_bucket_reclaim_if_empty(worker, bucket);
    return true;
}

// xr_worker_wake_all - Wake all coroutines waiting on specified Channel on current Worker
// (lock-free) MUST only be called from the owning worker thread.
void xr_worker_wake_all(XrWorker *worker, void *channel) {
    if (!worker || !channel)
        return;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "wake_all: cross-worker call detected (use chan_wake_queue)");

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket)
        return;

    XrCoroutine *ready_first = NULL;
    XrCoroutine *ready_last = NULL;
    int removed_count = 0;

    // Wake all senders
    XrCoroutine *coro = bucket->send_head;
    while (coro) {
        XrCoroutine *next = coro->ext->wait_link;
        if (worker_blocked_list_remove(worker, coro)) {
            removed_count++;
        }

        bucket_clear_coro_links(coro);
        // Claim the wake atomically: a racing waker (e.g. channel_wake_coro_ex
        // on the close path) may have already taken this coro BLOCKED->READY.
        // Only the claim winner enqueues, so the coro is never double-pushed.
        if (xr_coro_claim_wake(coro)) {
            xr_channel_wait_token_resolve(&coro->ext->chan_wait_token);
            atomic_store_explicit(&coro->ext->wait_channel, NULL, memory_order_relaxed);
            worker_cancel_coro_timer_wait(worker, coro);
            xr_coro_resume_store(coro, XR_RESUME_CHANNEL_CLOSED);
            coro->sched_link = NULL;
            if (ready_last) {
                ready_last->sched_link = coro;
            } else {
                ready_first = coro;
            }
            ready_last = coro;
        }

        coro = next;
    }
    bucket->send_head = bucket->send_tail = NULL;

    // Wake all receivers
    coro = bucket->recv_head;
    while (coro) {
        XrCoroutine *next = coro->ext->wait_link;
        if (worker_blocked_list_remove(worker, coro)) {
            removed_count++;
        }

        bucket_clear_coro_links(coro);
        if (xr_coro_claim_wake(coro)) {
            xr_channel_wait_token_resolve(&coro->ext->chan_wait_token);
            atomic_store_explicit(&coro->ext->wait_channel, NULL, memory_order_relaxed);
            worker_cancel_coro_timer_wait(worker, coro);
            xr_coro_resume_store(coro, XR_RESUME_CHANNEL_CLOSED);
            coro->sched_link = NULL;
            if (ready_last) {
                ready_last->sched_link = coro;
            } else {
                ready_first = coro;
            }
            ready_last = coro;
        }

        coro = next;
    }
    bucket->recv_head = bucket->recv_tail = NULL;
    if (removed_count > 0) {
        worker->p.blocked_count -= removed_count;
    }
    if (ready_first) {
        (void) xr_worker_push_batch(worker, ready_first);
    }
    worker_clear_channel_waiter_mask(worker, channel);

    // Reclaim the bucket if select_head is also empty.
    worker_blocked_bucket_reclaim_if_empty(worker, bucket);
}
