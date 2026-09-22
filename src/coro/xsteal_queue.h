/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsteal_queue.h - Lock-free work-stealing queue
 *
 * KEY CONCEPT:
 *   Chase-Lev deque for million-coroutine load balancing.
 *   Local ops (push/pop) lock-free, steal uses single CAS.
 *   Owner pops LIFO from the bottom; thieves steal FIFO from the top.
 *   Fixed-capacity ring (set once at init, clamped to XR_STEAL_QUEUE_MAX_SIZE);
 *   it does NOT resize — push returns false when full and the caller spills the
 *   overflow to the per-worker XrRunQueue.
 */

#ifndef XSTEAL_QUEUE_H
#define XSTEAL_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "../base/xdefs.h"

// Forward declaration
struct XrCoroutine;

// ========== Configuration ==========

#define XR_STEAL_QUEUE_DEFAULT_SIZE 256    // Must be power of 2
#define XR_STEAL_QUEUE_MAX_SIZE (1 << 20)  // 1M

// ========== Steal Queue Structure ==========

typedef struct XrStealQueueSlot {
    _Atomic(struct XrCoroutine *) coro;
    _Atomic int64_t submit_time;
} XrStealQueueSlot;

// Lock-free work-stealing queue (Chase-Lev deque)
// push/pop: owner thread, steal: other threads
typedef struct XrStealQueue {
    XrStealQueueSlot *buffer;               // Ring buffer and owned freshness hints
    _Atomic int64_t top;                    // Steal end (head)
    _Atomic int64_t bottom;                 // Local end (tail)
    int64_t mask;                           // Size mask
    int capacity;
} XrStealQueue;

typedef enum XrStealQueueStatus {
    XR_STEAL_QUEUE_EMPTY = 0,
    XR_STEAL_QUEUE_RETRY = 1,
    XR_STEAL_QUEUE_SUCCESS = 2,
} XrStealQueueStatus;

// ========== Lifecycle API ==========

XR_FUNC bool xr_steal_queue_init(XrStealQueue *q, int capacity);
XR_FUNC void xr_steal_queue_destroy(XrStealQueue *q);

// ========== Local Operations (lock-free) ==========

// Push coroutine (owner thread only, lock-free)
// Returns false if queue is full (caller should handle overflow)
XR_FUNC bool xr_steal_queue_push(XrStealQueue *q, struct XrCoroutine *coro, int64_t submit_time);

// Pop coroutine (owner thread only, LIFO)
XR_FUNC struct XrCoroutine *xr_steal_queue_pop(XrStealQueue *q);

// ========== Steal Operations (CAS) ==========

// Steal with status so callers can distinguish empty from CAS contention.
XR_FUNC XrStealQueueStatus xr_steal_queue_steal_status(XrStealQueue *q,
                                                       struct XrCoroutine **out_coro);

// ========== Status Query ==========

// Get queue size (approximate)
XR_FUNC int xr_steal_queue_size(XrStealQueue *q);

// Check if empty
XR_FUNC bool xr_steal_queue_empty(XrStealQueue *q);

// Diagnostic snapshot: read entries without removing them (best-effort, not atomic)
// Returns number of entries written to out_buf (up to max_out)
XR_FUNC int xr_steal_queue_snapshot(XrStealQueue *q, struct XrCoroutine **out_buf, int max_out);

/* Freshness scans borrow queue storage, never a coroutine. Only a successful
 * pop or steal
 * transfers the right to dereference a queued shell. Ring reuse
 * may make this hint stale, but
 * cannot expose a reclaimed shell. */
static inline bool xr_steal_queue_peek_time(XrStealQueue *q, int64_t *out_time) {
    if (!q || !q->buffer || !out_time)
        return false;
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);
    if (t >= b)
        return false;
    int64_t time = atomic_load_explicit(&q->buffer[t & q->mask].submit_time, memory_order_relaxed);
    if (atomic_load_explicit(&q->top, memory_order_acquire) != t)
        return false;
    *out_time = time;
    return true;
}

#endif  // XSTEAL_QUEUE_H
