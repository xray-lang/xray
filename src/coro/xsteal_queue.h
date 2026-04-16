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
 *   FIFO: push to tail, steal from head. Dynamic resize supported.
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

#define XR_STEAL_QUEUE_DEFAULT_SIZE     256        // Must be power of 2
#define XR_STEAL_QUEUE_MAX_SIZE         (1 << 20)  // 1M

// ========== Steal Queue Structure ==========

// Lock-free work-stealing queue (Chase-Lev deque)
// push/pop: owner thread, steal: other threads
typedef struct XrStealQueue {
    _Atomic(struct XrCoroutine*) *buffer;  // Ring buffer
    _Atomic int64_t top;                   // Steal end (head)
    _Atomic int64_t bottom;                // Local end (tail)
    int64_t mask;                          // Size mask
    int capacity;
} XrStealQueue;

// ========== Lifecycle API ==========

XR_FUNC bool xr_steal_queue_init(XrStealQueue *q, int capacity);
XR_FUNC void xr_steal_queue_destroy(XrStealQueue *q);

// ========== Local Operations (lock-free) ==========

// Push coroutine (owner thread only, lock-free)
// Returns false if queue is full (caller should handle overflow)
XR_FUNC bool xr_steal_queue_push(XrStealQueue *q, struct XrCoroutine *coro);

// Pop coroutine (owner thread only, LIFO)
XR_FUNC struct XrCoroutine* xr_steal_queue_pop(XrStealQueue *q);

// ========== Steal Operations (CAS) ==========

// Steal coroutine (other threads, CAS, FIFO)
XR_FUNC struct XrCoroutine* xr_steal_queue_steal(XrStealQueue *q);

// ========== Status Query ==========

// Get queue size (approximate)
XR_FUNC int xr_steal_queue_size(XrStealQueue *q);

// Check if empty
XR_FUNC bool xr_steal_queue_empty(XrStealQueue *q);

// Diagnostic snapshot: read entries without removing them (best-effort, not atomic)
// Returns number of entries written to out_buf (up to max_out)
XR_FUNC int xr_steal_queue_snapshot(XrStealQueue *q, struct XrCoroutine **out_buf, int max_out);

// ========== Non-destructive Peek (for time-aware stealing) ==========

// Peek at the oldest item (steal end) without removing it.
// Racy but safe as a freshness hint — worst case we see a stale item.
static inline struct XrCoroutine *xr_steal_queue_peek_top(XrStealQueue *q) {
    if (!q || !q->buffer) return NULL;
    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    if (t >= b) return NULL;
    return atomic_load_explicit(&q->buffer[t & q->mask], memory_order_relaxed);
}

#endif // XSTEAL_QUEUE_H
