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
 *     a time (matches wait_channel + wait_send state).
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
    return (int)(xr_hash_int((int64_t)(intptr_t)channel) % XR_BLOCKED_BUCKET_SIZE);
}

// Per-Worker version: find or create blocked bucket for Channel (lock-free)
XrBlockedBucket *worker_blocked_bucket_find_or_create(XrWorker *worker, void *channel) {
    int idx = blocked_bucket_hash(channel);
    XrBlockedBucket *bucket = worker->p.blocked_buckets[idx];

    while (bucket) {
        if (bucket->channel == channel) return bucket;
        bucket = bucket->next;
    }

    bucket = (XrBlockedBucket *)xr_malloc(sizeof(XrBlockedBucket));
    if (!bucket) return NULL;

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
        if (bucket->channel == channel) return bucket;
        bucket = bucket->next;
    }
    return NULL;
}

// True if the bucket holds no waiter at all — eligible for reclamation.
// send_head / recv_head are populated by xr_worker_block; select_head is
// populated by xr_worker_block_select (xworker_sysmon.c).
static inline bool bucket_is_empty(const XrBlockedBucket *b) {
    return b->send_head == NULL
        && b->recv_head == NULL
        && b->select_head == NULL;
}

// Reclaim an empty bucket: unlink it from the per-worker hash chain and
// free the malloc'd memory. Owner-private, no lock needed.
//
// Prior to this helper the hash chain grew monotonically for every distinct
// channel ever blocked on — a slow leak that could also lengthen lookup time
// after many short-lived channels had been garbage-collected.
static void bucket_reclaim_if_empty(XrWorker *worker, XrBlockedBucket *bucket) {
    if (!bucket || !bucket_is_empty(bucket)) return;

    int idx = blocked_bucket_hash(bucket->channel);
    XrBlockedBucket **pp = &worker->p.blocked_buckets[idx];
    while (*pp) {
        if (*pp == bucket) {
            *pp = bucket->next;
            xr_free(bucket);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Per-Worker version: add coroutine to linear blocked queue (lock-free)
void worker_blocked_list_add(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;

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
    if (!coro) return false;

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
    if (!worker || !coro) return;

    // Prevent duplicate add: if coroutine already in blocked queue, return
    if (coro->next != NULL || coro->prev != NULL || worker->p.blocked_head == coro) {
        return;
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

    // If has Channel, add to hash table (use wait_link to avoid conflict with sched_link/MPSC)
    if (coro->wait_channel) {
        XrBlockedBucket *bucket = worker_blocked_bucket_find_or_create(worker, coro->wait_channel);
        if (bucket) {
            if (coro->wait_send) {
                coro->wait_link = NULL;
                if (bucket->send_tail) {
                    bucket->send_tail->wait_link = coro;
                } else {
                    bucket->send_head = coro;
                }
                bucket->send_tail = coro;
            } else {
                coro->wait_link = NULL;
                if (bucket->recv_tail) {
                    bucket->recv_tail->wait_link = coro;
                } else {
                    bucket->recv_head = coro;
                }
                bucket->recv_tail = coro;
            }
        }
    }

    worker->p.blocked_count++;
}

// xr_worker_unblock - Remove coroutine from Worker's blocked queue (lock-free)
void xr_worker_unblock(XrWorker *worker, XrCoroutine *coro) {
    if (!worker || !coro) return;

    if (worker_blocked_list_remove(worker, coro)) {
        worker->p.blocked_count--;
    }
}

// xr_worker_wake_one - Wake one coroutine waiting on specified Channel on current Worker (lock-free)
// MUST only be called from the owning worker thread.
XrCoroutine *xr_worker_wake_one(XrWorker *worker, void *channel, bool wake_sender) {
    if (!worker || !channel) return NULL;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "wake_one: cross-worker call detected (use chan_wake_queue)");

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket) return NULL;

    XrCoroutine *coro = NULL;
    if (wake_sender) {
        coro = bucket->send_head;
        if (coro) {
            bucket->send_head = coro->wait_link;
            if (!bucket->send_head) bucket->send_tail = NULL;
        }
    } else {
        coro = bucket->recv_head;
        if (coro) {
            bucket->recv_head = coro->wait_link;
            if (!bucket->recv_head) bucket->recv_tail = NULL;
        }
    }

    if (!coro) return NULL;

    // Remove from linear queue
    worker_blocked_list_remove(worker, coro);
    worker->p.blocked_count--;

    // Clear blocked info
    coro->wait_channel = NULL;
    coro->wait_link = NULL;

    // Cancel timer (sendTimeout/recvTimeout scenario)
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
    }

    // Critical: set resume status so instruction detects resume from Channel block
    xr_coro_resume_store(coro, XR_RESUME_CHANNEL);

    // Set ready state and add to this Worker's LIFO slot for locality
    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
    xr_coro_flags_set(coro, XR_CORO_FLG_READY);
    xr_worker_push_lifo(worker, coro);

    // Reclaim the bucket if this was the last waiter on the channel.
    bucket_reclaim_if_empty(worker, bucket);

    return coro;
}

// xr_worker_dequeue_blocked - Dequeue coroutine from blocked queue but don't enqueue to run queue
// For rendezvous value passing: caller needs to process value first then manually enqueue
XrCoroutine *xr_worker_dequeue_blocked(XrWorker *worker, void *channel, bool wake_sender) {
    if (!worker || !channel) return NULL;

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket) return NULL;

    XrCoroutine *coro = NULL;
    if (wake_sender) {
        coro = bucket->send_head;
        if (coro) {
            bucket->send_head = coro->wait_link;
            if (!bucket->send_head) bucket->send_tail = NULL;
        }
    } else {
        coro = bucket->recv_head;
        if (coro) {
            bucket->recv_head = coro->wait_link;
            if (!bucket->recv_head) bucket->recv_tail = NULL;
        }
    }

    if (!coro) return NULL;

    // Remove from linear queue
    worker_blocked_list_remove(worker, coro);
    worker->p.blocked_count--;

    // Clear blocked info, but don't enqueue (caller responsible)
    coro->wait_channel = NULL;
    coro->wait_link = NULL;
    xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);

    // Reclaim the bucket if this was the last waiter on the channel.
    bucket_reclaim_if_empty(worker, bucket);

    return coro;
}

// xr_worker_wake_all - Wake all coroutines waiting on specified Channel on current Worker (lock-free)
// MUST only be called from the owning worker thread.
void xr_worker_wake_all(XrWorker *worker, void *channel) {
    if (!worker || !channel) return;
    XR_DCHECK(xr_current_worker() == NULL || xr_current_worker() == worker,
              "wake_all: cross-worker call detected (use chan_wake_queue)");

    XrBlockedBucket *bucket = worker_blocked_bucket_find(worker, channel);
    if (!bucket) return;

    // Wake all senders
    XrCoroutine *coro = bucket->send_head;
    while (coro) {
        XrCoroutine *next = coro->wait_link;
        worker_blocked_list_remove(worker, coro);
        worker->p.blocked_count--;

        coro->wait_channel = NULL;
        coro->wait_link = NULL;
        // Guard: skip coros already woken by channel_wake_coro_ex (close path).
        // channel_wake_coro_ex clears BLOCKED before we get here; pushing a
        // non-BLOCKED coro would double-enqueue it in the run queue.
        if (xr_coro_flags_has(coro, XR_CORO_FLG_BLOCKED)) {
            xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
            xr_worker_push(worker, coro);
        }

        coro = next;
    }
    bucket->send_head = bucket->send_tail = NULL;

    // Wake all receivers
    coro = bucket->recv_head;
    while (coro) {
        XrCoroutine *next = coro->wait_link;
        worker_blocked_list_remove(worker, coro);
        worker->p.blocked_count--;

        coro->wait_channel = NULL;
        coro->wait_link = NULL;
        // Guard: skip coros already woken (see sender guard comment above)
        if (xr_coro_flags_has(coro, XR_CORO_FLG_BLOCKED)) {
            xr_coro_flags_clear(coro, XR_CORO_FLG_BLOCKED | XR_CORO_WAIT_MASK);
            xr_coro_flags_set(coro, XR_CORO_FLG_READY);
            xr_worker_push(worker, coro);
        }

        coro = next;
    }
    bucket->recv_head = bucket->recv_tail = NULL;

    // Reclaim the bucket if select_head is also empty.
    bucket_reclaim_if_empty(worker, bucket);
}
