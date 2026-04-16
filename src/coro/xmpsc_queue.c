/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmpsc_queue.c - Lock-free MPSC queue (Treiber stack)
 *
 * KEY CONCEPT:
 *   Treiber stack: push prepends via CAS, drain swaps head to NULL.
 *   sched_link is set BEFORE the CAS succeeds, so the consumer
 *   always sees a fully-linked chain. No reverse needed.
 */

#include "xmpsc_queue.h"
#include "../base/xchecks.h"
#include "xcoroutine.h"

void xr_mpsc_init(XrMPSCQueue *q) {
    atomic_store(&q->head, NULL);
}

// Push: CAS loop to prepend node to stack
// sched_link points to old head BEFORE CAS succeeds — race-free
void xr_mpsc_push(XrMPSCQueue *q, struct XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "mpsc_push: NULL queue");
    XR_DCHECK(coro != NULL, "mpsc_push: NULL coro");
    struct XrCoroutine *old_head;
    do {
        old_head = atomic_load_explicit(&q->head, memory_order_relaxed);
        coro->sched_link = old_head;
    } while (!atomic_compare_exchange_weak_explicit(
        &q->head, &old_head, coro,
        memory_order_release, memory_order_relaxed));
}

// Drain: O(1) atomic swap, returns list via sched_link (newest first)
struct XrCoroutine* xr_mpsc_drain(XrMPSCQueue *q) {
    return atomic_exchange_explicit(&q->head, NULL, memory_order_acquire);
}

bool xr_mpsc_empty(XrMPSCQueue *q) {
    return atomic_load_explicit(&q->head, memory_order_relaxed) == NULL;
}
