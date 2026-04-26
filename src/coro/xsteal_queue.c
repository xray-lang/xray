/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsteal_queue.c - Lock-free work-stealing queue implementation
 *
 * KEY CONCEPT:
 *   Based on Chase-Lev deque algorithm.
 *   Ref: "Dynamic Circular Work-Stealing Deque" - Chase & Lev, SPAA 2005
 */

#include "xsteal_queue.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>

// ========== Helper Functions ==========

// Round up to power of 2
static int next_power_of_two(int n) {
    if (n <= 0)
        return 1;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

// ========== Lifecycle API ==========

bool xr_steal_queue_init(XrStealQueue *q, int capacity) {
    if (!q)
        return false;

    memset(q, 0, sizeof(XrStealQueue));

    // Use default capacity
    if (capacity <= 0) {
        capacity = XR_STEAL_QUEUE_DEFAULT_SIZE;
    }

    // Round up to power of 2
    capacity = next_power_of_two(capacity);

    // Limit max capacity
    if (capacity > XR_STEAL_QUEUE_MAX_SIZE) {
        capacity = XR_STEAL_QUEUE_MAX_SIZE;
    }

    // Allocate buffer
    q->buffer = xr_calloc(capacity, sizeof(_Atomic(struct XrCoroutine *)));
    if (!q->buffer) {
        return false;
    }

    q->capacity = capacity;
    q->mask = capacity - 1;
    XR_DCHECK((capacity & (capacity - 1)) == 0, "steal_queue_init: capacity not power-of-2");
    XR_DCHECK(q->mask == (uint64_t) (capacity - 1), "steal_queue_init: mask mismatch");
    atomic_store(&q->top, 0);
    atomic_store(&q->bottom, 0);

    return true;
}

void xr_steal_queue_destroy(XrStealQueue *q) {
    if (!q)
        return;

    if (q->buffer) {
        xr_free(q->buffer);
        q->buffer = NULL;
    }

    q->capacity = 0;
    q->mask = 0;
}

// ========== Local Operations ==========

bool xr_steal_queue_push(XrStealQueue *q, struct XrCoroutine *coro) {
    if (!q || !q->buffer || !coro)
        return false;

    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);

    // Queue full: caller must handle overflow
    if (b - t >= q->capacity) {
        return false;
    }

    // Write data
    atomic_store_explicit(&q->buffer[b & q->mask], coro, memory_order_relaxed);

    // Memory fence: ensure data write visible to other threads
    atomic_thread_fence(memory_order_release);

    // Update bottom
    atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    return true;
}

struct XrCoroutine *xr_steal_queue_pop(XrStealQueue *q) {
    if (!q || !q->buffer)
        return NULL;

    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed) - 1;

    // Update bottom first (declare intent)
    atomic_store_explicit(&q->bottom, b, memory_order_relaxed);

    // Full fence: ensure bottom update visible to stealers
    atomic_thread_fence(memory_order_seq_cst);

    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);

    struct XrCoroutine *coro = NULL;

    if (t <= b) {
        // Queue not empty
        coro = atomic_load_explicit(&q->buffer[b & q->mask], memory_order_relaxed);

        if (t == b) {
            // Only one element left, possible stealer contention
            if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst,
                                                         memory_order_relaxed)) {
                // CAS failed: stolen
                coro = NULL;
            }
            // Restore bottom (success or not)
            atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        // Queue empty, restore bottom
        atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    }

    return coro;
}

// ========== Steal Operations ==========

struct XrCoroutine *xr_steal_queue_steal(XrStealQueue *q) {
    if (!q || !q->buffer)
        return NULL;

    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);

    // Memory fence: ensure reading top before bottom
    atomic_thread_fence(memory_order_seq_cst);

    int64_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);

    if (t >= b) {
        // Queue empty
        return NULL;
    }

    // Read data
    struct XrCoroutine *coro = atomic_load_explicit(&q->buffer[t & q->mask], memory_order_relaxed);

    // CAS attempt to steal
    if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst,
                                                 memory_order_relaxed)) {
        // CAS failed: contention, return NULL for caller to retry
        return NULL;
    }

    return coro;
}

// ========== State Query ==========

int xr_steal_queue_size(XrStealQueue *q) {
    if (!q)
        return 0;

    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);

    int64_t size = b - t;
    return size > 0 ? (int) size : 0;
}

bool xr_steal_queue_empty(XrStealQueue *q) {
    return xr_steal_queue_size(q) <= 0;
}

// Diagnostic snapshot: read buffer entries without removing them
// Best-effort: entries may be in-flight during concurrent push/pop/steal
int xr_steal_queue_snapshot(XrStealQueue *q, struct XrCoroutine **out_buf, int max_out) {
    if (!q || !q->buffer || !out_buf || max_out <= 0)
        return 0;

    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int count = 0;

    for (int64_t i = t; i < b && count < max_out; i++) {
        struct XrCoroutine *c = atomic_load_explicit(&q->buffer[i & q->mask], memory_order_relaxed);
        if (c) {
            out_buf[count++] = c;
        }
    }
    return count;
}
