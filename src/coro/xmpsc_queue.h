/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmpsc_queue.h - Lock-free MPSC queue
 *
 * KEY CONCEPT:
 *   Multi-producer single-consumer lock-free queue for cross-scheduler
 *   coroutine enqueuing. Based on Dmitry Vyukov's MPSC queue algorithm.
 */

#ifndef XMPSC_QUEUE_H
#define XMPSC_QUEUE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "../base/xdefs.h"

// Forward declaration
struct XrCoroutine;

// Lock-free MPSC queue (Treiber stack)
// Push: CAS to prepend (sched_link set before visible — race-free)
// Drain: O(1) atomic swap, no reverse needed
typedef struct XrMPSCQueue {
    _Atomic(struct XrCoroutine*) head;  // Stack top (newest pushed)
} XrMPSCQueue;

// Initialize queue
XR_FUNC void xr_mpsc_init(XrMPSCQueue *q);

// Push (thread-safe, CAS loop)
XR_FUNC void xr_mpsc_push(XrMPSCQueue *q, struct XrCoroutine *coro);

// Drain all elements via O(1) atomic swap, return list via sched_link
XR_FUNC struct XrCoroutine* xr_mpsc_drain(XrMPSCQueue *q);

// Check if empty (approximate)
XR_FUNC bool xr_mpsc_empty(XrMPSCQueue *q);

#endif // XMPSC_QUEUE_H
