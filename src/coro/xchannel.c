/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchannel.c - CSP Channel implementation
 *
 * KEY CONCEPT:
 *   Unbuffered channels use synchronous handshake semantics.
 *   Buffered channels use FIFO queue. Deep copy ensures lock-free GC.
 *
 * WHY THIS DESIGN:
 *   - Lock-free fast path for common cases
 *   - Direct transfer optimization when receiver waiting
 *   - Deep copy on cross-coroutine transfer (xray specific)
 */

#include "xchannel.h"
#include "xblock.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xisolate_internal.h"
#include "../base/xchecks.h"
#include "xdeep_copy.h"
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/xshared.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Forward-declare wake helper (defined later in this file) so that
// the timer_channel_fire_cb callback at the top can reference it.
static void channel_wake_coro(XrCoroutine *coro);

static XrRuntime *channel_stats_runtime(XrChannel *ch) {
    if (!ch || !ch->isolate || !ch->isolate->vm.runtime)
        return NULL;
    XrRuntime *runtime = (XrRuntime *) ch->isolate->vm.runtime;
    return XR_UNLIKELY(runtime->sched_stats_enabled) ? runtime : NULL;
}

#define CHANNEL_METRIC_INC(ch, field)                                                              \
    do {                                                                                           \
        XrRuntime *_rt = channel_stats_runtime((ch));                                              \
        if (_rt)                                                                                   \
            xr_sched_metric_inc(_rt, &_rt->sched_stats.field);                                     \
    } while (0)

#define CHANNEL_METRIC_ADD(ch, field, value)                                                       \
    do {                                                                                           \
        XrRuntime *_rt = channel_stats_runtime((ch));                                              \
        if (_rt)                                                                                   \
            xr_sched_metric_add(_rt, &_rt->sched_stats.field, (value));                            \
    } while (0)

XR_FUNC void xr_channel_lock_observed(XrChannel *ch) {
    XR_DCHECK(ch != NULL, "channel_lock_observed: NULL channel");
    XrRuntime *runtime = channel_stats_runtime(ch);
    if (runtime) {
        if (xr_amutex_trylock(&ch->lock)) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_fast_count);
            return;
        }
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_contended_count);
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_slow_count);
    }
    xr_amutex_lock(&ch->lock);
}

static bool channel_trylock_observed(XrChannel *ch) {
    XR_DCHECK(ch != NULL, "channel_trylock_observed: NULL channel");
    bool locked = xr_amutex_trylock(&ch->lock);
    XrRuntime *runtime = channel_stats_runtime(ch);
    if (runtime) {
        xr_sched_metric_inc(runtime, locked ? &runtime->sched_stats.chan_lock_fast_count
                                            : &runtime->sched_stats.chan_lock_contended_count);
    }
    return locked;
}

static void channel_lock_after_failed_try(XrChannel *ch) {
    XR_DCHECK(ch != NULL, "channel_lock_after_failed_try: NULL channel");
    XrRuntime *runtime = channel_stats_runtime(ch);
    if (!runtime) {
        xr_amutex_lock(&ch->lock);
        return;
    }

    xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_slow_count);
    XrAdaptiveMutexAcquireMode mode = xr_amutex_lock_profiled(&ch->lock);
    if (mode == XR_AMUTEX_ACQUIRE_FAST) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_recheck_count);
    } else if (mode == XR_AMUTEX_ACQUIRE_SPIN) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_spin_count);
    } else if (mode == XR_AMUTEX_ACQUIRE_YIELD) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_yield_count);
    } else if (mode == XR_AMUTEX_ACQUIRE_SLEEP) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_lock_sleep_count);
    }
}

// Ring buffer index advance: conditional increment is faster than modulo division
// on ARM64 (~1-2 cycles vs ~10 cycles for SDIV+MSUB)
static inline uint32_t chan_advance_idx(uint32_t idx, uint32_t buf_size) {
    return (++idx >= buf_size) ? 0 : idx;
}

static inline bool channel_single_worker(uint64_t mask) {
    return mask != 0 && (mask & (mask - 1)) == 0;
}

static XrChannelKind channel_infer_worker_kind(XrChannel *ch) {
    if (!ch)
        return XR_CHAN_GENERIC;
    if (ch->buf_size == 0)
        return XR_CHAN_RENDEZVOUS;

    uint64_t producers = ch->producer_worker_mask;
    uint64_t consumers = ch->consumer_worker_mask;
    if (producers == 0 || consumers == 0)
        return XR_CHAN_GENERIC;
    if (channel_single_worker(producers) && channel_single_worker(consumers))
        return XR_CHAN_SPSC;
    if (!channel_single_worker(producers) && channel_single_worker(consumers))
        return XR_CHAN_MPSC;
    if (channel_single_worker(producers) && !channel_single_worker(consumers))
        return XR_CHAN_WORK_QUEUE;
    return XR_CHAN_MPMC;
}

static inline bool channel_kind_observation_stable(XrChannelKind kind) {
    return kind == XR_CHAN_MPMC || kind == XR_CHAN_RENDEZVOUS;
}

static XrChannelKind channel_infer_role_kind(XrChannel *ch) {
    if (!ch)
        return XR_CHAN_GENERIC;
    if (ch->buf_size == 0)
        return XR_CHAN_RENDEZVOUS;

    if (ch->producer_coro_id < 0 || ch->consumer_coro_id < 0)
        return XR_CHAN_GENERIC;
    if (!ch->producer_coro_multi && !ch->consumer_coro_multi)
        return XR_CHAN_SPSC;
    if (ch->producer_coro_multi && !ch->consumer_coro_multi)
        return XR_CHAN_MPSC;
    if (!ch->producer_coro_multi && ch->consumer_coro_multi)
        return XR_CHAN_WORK_QUEUE;
    return XR_CHAN_MPMC;
}

static void channel_record_kind_metric(XrRuntime *runtime, XrChannelKind kind, bool worker_kind) {
    if (!runtime)
        return;
    XrSchedGlobalStats *stats = &runtime->sched_stats;
    if (worker_kind) {
        if (kind == XR_CHAN_SPSC) {
            xr_sched_metric_inc(runtime, &stats->chan_worker_kind_spsc_count);
        } else if (kind == XR_CHAN_MPSC) {
            xr_sched_metric_inc(runtime, &stats->chan_worker_kind_mpsc_count);
        } else if (kind == XR_CHAN_WORK_QUEUE) {
            xr_sched_metric_inc(runtime, &stats->chan_worker_kind_work_queue_count);
        } else if (kind == XR_CHAN_MPMC) {
            xr_sched_metric_inc(runtime, &stats->chan_worker_kind_mpmc_count);
        }
        return;
    }

    if (kind == XR_CHAN_SPSC) {
        xr_sched_metric_inc(runtime, &stats->chan_kind_spsc_count);
    } else if (kind == XR_CHAN_MPSC) {
        xr_sched_metric_inc(runtime, &stats->chan_kind_mpsc_count);
    } else if (kind == XR_CHAN_WORK_QUEUE) {
        xr_sched_metric_inc(runtime, &stats->chan_kind_work_queue_count);
    } else if (kind == XR_CHAN_MPMC) {
        xr_sched_metric_inc(runtime, &stats->chan_kind_mpmc_count);
    }
}

static void channel_record_kind_op_metric(XrRuntime *runtime, XrChannelKind kind,
                                          bool worker_kind) {
    if (!runtime)
        return;
    XrSchedGlobalStats *stats = &runtime->sched_stats;
    _Atomic uint64_t *counter = NULL;
    if (worker_kind) {
        switch (kind) {
            case XR_CHAN_GENERIC:
                counter = &stats->chan_worker_kind_generic_op_count;
                break;
            case XR_CHAN_SPSC:
                counter = &stats->chan_worker_kind_spsc_op_count;
                break;
            case XR_CHAN_MPSC:
                counter = &stats->chan_worker_kind_mpsc_op_count;
                break;
            case XR_CHAN_WORK_QUEUE:
                counter = &stats->chan_worker_kind_work_queue_op_count;
                break;
            case XR_CHAN_MPMC:
                counter = &stats->chan_worker_kind_mpmc_op_count;
                break;
            case XR_CHAN_RENDEZVOUS:
                counter = &stats->chan_worker_kind_rendezvous_op_count;
                break;
        }
    } else {
        switch (kind) {
            case XR_CHAN_GENERIC:
                counter = &stats->chan_kind_generic_op_count;
                break;
            case XR_CHAN_SPSC:
                counter = &stats->chan_kind_spsc_op_count;
                break;
            case XR_CHAN_MPSC:
                counter = &stats->chan_kind_mpsc_op_count;
                break;
            case XR_CHAN_WORK_QUEUE:
                counter = &stats->chan_kind_work_queue_op_count;
                break;
            case XR_CHAN_MPMC:
                counter = &stats->chan_kind_mpmc_op_count;
                break;
            case XR_CHAN_RENDEZVOUS:
                counter = &stats->chan_kind_rendezvous_op_count;
                break;
        }
    }
    if (counter)
        xr_sched_metric_inc(runtime, counter);
}

static XrCoroutine *channel_current_coro_from_worker(XrWorker *worker) {
    if (!worker || !worker->m)
        return NULL;
    return atomic_load_explicit(&worker->m->current_coro, memory_order_relaxed);
}

static void channel_note_worker_locked(XrChannel *ch, XrWorker *worker, bool producer) {
    if (!ch || !worker || channel_kind_observation_stable(ch->worker_kind))
        return;

    uint64_t bit = xr_runtime_worker_bit(worker->p.id);
    if (bit == 0)
        return;

    uint64_t *mask = producer ? &ch->producer_worker_mask : &ch->consumer_worker_mask;
    if ((*mask & bit) != 0)
        return;

    *mask |= bit;
    XrChannelKind next = channel_infer_worker_kind(ch);
    if (next == ch->worker_kind)
        return;

    ch->worker_kind = next;
    channel_record_kind_metric(worker->p.runtime, next, true);
}

static bool channel_note_role_id_locked(XrChannel *ch, int coro_id, bool producer) {
    if (!ch || coro_id < 0)
        return false;

    int *seen_id = producer ? &ch->producer_coro_id : &ch->consumer_coro_id;
    bool *multi = producer ? &ch->producer_coro_multi : &ch->consumer_coro_multi;
    if (*multi)
        return false;
    if (*seen_id < 0) {
        *seen_id = coro_id;
        return true;
    }
    if (*seen_id == coro_id)
        return false;

    *multi = true;
    return true;
}

static void channel_note_participant_locked(XrChannel *ch, XrCoroutine *coro, bool producer) {
    if (!ch)
        return;

    XrWorker *worker = xr_current_worker();
    XrRuntime *runtime = worker ? worker->p.runtime : channel_stats_runtime(ch);
    channel_note_worker_locked(ch, worker, producer);
    if (channel_kind_observation_stable(ch->kind)) {
        channel_record_kind_op_metric(runtime, ch->kind, false);
        channel_record_kind_op_metric(runtime, ch->worker_kind, true);
        return;
    }

    if (!coro)
        coro = channel_current_coro_from_worker(worker);
    if (!channel_note_role_id_locked(ch, coro ? coro->id : -1, producer)) {
        channel_record_kind_op_metric(runtime, ch->kind, false);
        channel_record_kind_op_metric(runtime, ch->worker_kind, true);
        return;
    }

    XrChannelKind next = channel_infer_role_kind(ch);
    if (next == ch->kind) {
        channel_record_kind_op_metric(runtime, ch->kind, false);
        channel_record_kind_op_metric(runtime, ch->worker_kind, true);
        return;
    }

    ch->kind = next;
    channel_record_kind_metric(runtime, next, false);
    channel_record_kind_op_metric(runtime, ch->kind, false);
    channel_record_kind_op_metric(runtime, ch->worker_kind, true);
}

// Distributed channel hooks live on XrayIsolate::channel_dist_hooks.
// Fetch via channel->isolate->channel_dist_hooks in the functions below.
//
// Channel close counter lives on XrSystemHeap::stats.channel_close_count
// (mirrors channel_create_count; see xsystem_heap.h). Access via the
// isolate-aware xr_channel_get_close_count(X) below.

uint64_t xr_channel_get_close_count(struct XrayIsolate *X) {
    if (!X || !xr_isolate_get_sys_heap(X))
        return 0;
    return atomic_load(&xr_isolate_get_sys_heap(X)->stats.channel_close_count);
}

// ========== Wait Queue Implementation ==========

// Uses coroutine's channel wait links. Worker blocked buckets use a separate
// link pair, so timeout and remote-wake bookkeeping cannot corrupt channel FIFO
// ordering.
// Coroutine must be in BLOCKED state when enqueued.

void xr_waitq_enqueue(XrWaitQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "waitq_enqueue: NULL queue");
    XR_DCHECK(coro != NULL, "waitq_enqueue: NULL coro");
    XR_DCHECK(coro->ext != NULL, "waitq_enqueue: coro has no ext");
    XR_DCHECK(coro->ext->chan_wait_queue == NULL, "waitq_enqueue: coro already in channel waitq");
    coro->ext->chan_wait_next = NULL;
    coro->ext->chan_wait_prev = q->last;
    coro->ext->chan_wait_queue = q;
    if (q->last) {
        q->last->ext->chan_wait_next = coro;
    } else {
        q->first = coro;
    }
    q->last = coro;
}

static void waitq_enqueue_front(XrWaitQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "waitq_enqueue_front: NULL queue");
    XR_DCHECK(coro != NULL, "waitq_enqueue_front: NULL coro");
    XR_DCHECK(coro->ext != NULL, "waitq_enqueue_front: coro has no ext");
    XR_DCHECK(coro->ext->chan_wait_queue == NULL,
              "waitq_enqueue_front: coro already in channel waitq");
    coro->ext->chan_wait_next = q->first;
    coro->ext->chan_wait_prev = NULL;
    coro->ext->chan_wait_queue = q;
    if (q->first) {
        q->first->ext->chan_wait_prev = coro;
    }
    q->first = coro;
    if (!q->last) {
        q->last = coro;
    }
}

XrCoroutine *xr_waitq_dequeue(XrWaitQueue *q) {
    XR_DCHECK(q != NULL, "waitq_dequeue: NULL queue");
    XrCoroutine *coro = q->first;
    if (coro) {
        q->first = coro->ext->chan_wait_next;
        if (q->first == NULL) {
            q->last = NULL;
        } else {
            q->first->ext->chan_wait_prev = NULL;
        }
        coro->ext->chan_wait_next = NULL;
        coro->ext->chan_wait_prev = NULL;
        coro->ext->chan_wait_queue = NULL;
    }
    return coro;
}

// Remove specific coroutine from wait queue (for timeout cancellation)
static bool xr_waitq_remove(XrWaitQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "waitq_remove: NULL queue");
    XR_DCHECK(coro != NULL, "waitq_remove: NULL coro");
    if (coro->ext->chan_wait_queue != q)
        return false;

    if (coro->ext->chan_wait_prev) {
        coro->ext->chan_wait_prev->ext->chan_wait_next = coro->ext->chan_wait_next;
    } else {
        q->first = coro->ext->chan_wait_next;
    }
    if (coro->ext->chan_wait_next) {
        coro->ext->chan_wait_next->ext->chan_wait_prev = coro->ext->chan_wait_prev;
    } else {
        q->last = coro->ext->chan_wait_prev;
    }
    coro->ext->chan_wait_next = NULL;
    coro->ext->chan_wait_prev = NULL;
    coro->ext->chan_wait_queue = NULL;
    return true;
}

// Remove coroutine from channel wait queue (called on timeout)
void xr_channel_remove_waiter(XrChannel *ch, XrCoroutine *coro) {
    if (!ch || !coro)
        return;

    xr_channel_lock_observed(ch);

    // Try to remove from send queue
    XrWaitQueue *q = coro->ext->chan_wait_queue;
    if (q == &ch->sendq) {
        xr_waitq_remove(&ch->sendq, coro);
    } else if (q == &ch->recvq) {
        xr_waitq_remove(&ch->recvq, coro);
    } else {
        // Stale timeout callbacks can arrive after a channel operation already
        // dequeued the coroutine.
        (void) xr_waitq_remove(&ch->sendq, coro);
        (void) xr_waitq_remove(&ch->recvq, coro);
    }

    xr_amutex_unlock(&ch->lock);
}

// ========== GC Callbacks ==========

// Check if buffer is inline (allocated together with channel)
static inline bool channel_buffer_is_inline(XrChannel *ch) {
    return ch->buffer == (XrValue *) (ch + 1);
}

// GC destroy: free buffer only if separately allocated
void xr_gc_destroy_channel(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    XrChannel *ch = (XrChannel *) obj;
    if (ch->buffer && !channel_buffer_is_inline(ch)) {
        xr_free(ch->buffer);
        ch->buffer = NULL;
    }
}

// ========== Channel Creation and Destruction ==========

// buffer_size = 0: unbuffered sync channel
// buffer_size > 0: buffered async channel
// Channel is always allocated on system heap (shared across coroutines)
XrChannel *xr_channel_new(struct XrayIsolate *X, uint32_t buffer_size) {
    if (!X || !xr_isolate_get_sys_heap(X))
        return NULL;

    // Single allocation: XrChannel + inline buffer (like Go's makechan)
    size_t alloc_size = sizeof(XrChannel) + (size_t) buffer_size * sizeof(XrValue);
    XrChannel *ch =
        (XrChannel *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), alloc_size, XR_TCHANNEL);
    if (!ch)
        return NULL;

    // Set initial refcount to 1
    xr_shared_set_refc(&ch->gc_header, 1);
    // Runtime-managed: lifetime owned by the shared atomic-RC, not the
    // compiler's per-coroutine RC. dup/drop become no-ops. See docs/design/706.
    XR_OBJ_SET_FLAG(&ch->gc_header, XR_OBJ_MANAGED);

    // xr_sysheap_alloc_shared already memset(0) the entire allocation.
    // All fields default to 0/NULL/false which is correct for:
    //   buffer(NULL), buf_size(0), buf_count(0), send_idx(0), recv_idx(0),
    //   sendq(NULL,NULL), recvq(NULL,NULL), closed(0), lock(UNLOCKED=0),
    //   is_timer(0), timer_*(0), elem_tid(0), dist(NULL), name(NULL).
    // Only set non-zero fields.
    if (buffer_size > 0) {
        ch->buffer = (XrValue *) (ch + 1);
        ch->buf_size = buffer_size;
    }
    ch->kind = buffer_size == 0 ? XR_CHAN_RENDEZVOUS : XR_CHAN_GENERIC;
    ch->worker_kind = ch->kind;
    ch->producer_coro_id = -1;
    ch->consumer_coro_id = -1;
    ch->isolate = X;

    atomic_fetch_add(&xr_isolate_get_sys_heap(X)->stats.channel_create_count, 1);
    return ch;
}

// Timer wheel callback for time.after.
// Fires once when the timeout elapses, writes current time to the channel
// buffer so that any subsequent recv/tryRecv finds data immediately.
// If a receiver is already blocked on this channel, wake it directly.
static void timer_channel_fire_cb(void *arg) {
    XrChannel *ch = (XrChannel *) arg;
    /* Timer wheel callbacks may, in principle, fire after the channel
     * scheduling slot has been drained on a different worker; tolerate
     * the missing pointer instead of asserting. */
    if (!ch)
        return;

    // Double-check: callback should only fire once
    if (atomic_load_explicit(&ch->timer_fired, memory_order_relaxed))
        return;

    int64_t now = xr_monotonic_ticks();
    XrCoroutine *receiver = NULL;

    xr_channel_lock_observed(ch);
    if (!atomic_load_explicit(&ch->timer_fired, memory_order_relaxed)) {
        // Try to hand value directly to a blocked receiver
        receiver = xr_waitq_dequeue(&ch->recvq);
        if (receiver) {
            (void) xr_coro_store_recv_value(receiver, xr_int(now));
        } else {
            // No receiver waiting: leave value in buffer for later recv
            ch->buffer[0] = xr_int(now);
            ch->buf_count = 1;
        }
        atomic_store_explicit(&ch->timer_fired, true, memory_order_release);
    }
    xr_amutex_unlock(&ch->lock);

    // Wake receiver outside lock (same pattern as chan_direct_send)
    if (receiver) {
        channel_wake_coro(receiver);
    }
}

// Create Timer Channel
// Returns a read-only channel that sends current time after timeout.
// Uses the channel's embedded tw_timer for timer wheel registration.
XrChannel *xr_channel_new_timer(struct XrayIsolate *X, int64_t timeout_ms) {
    if (!X || !xr_isolate_get_sys_heap(X))
        return NULL;

    // Single allocation: XrChannel + 1-element inline buffer
    size_t alloc_size = sizeof(XrChannel) + sizeof(XrValue);
    XrChannel *ch =
        (XrChannel *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), alloc_size, XR_TCHANNEL);
    if (!ch)
        return NULL;

    // Set initial refcount to 1
    xr_shared_set_refc(&ch->gc_header, 1);
    // Runtime-managed: see xr_channel_new. dup/drop become no-ops.
    XR_OBJ_SET_FLAG(&ch->gc_header, XR_OBJ_MANAGED);

    // Inline single-element buffer
    ch->buffer = (XrValue *) (ch + 1);
    ch->buffer[0] = xr_null();
    ch->buf_size = 1;
    ch->buf_count = 0;
    ch->send_idx = 0;
    ch->recv_idx = 0;
    ch->kind = XR_CHAN_GENERIC;
    ch->worker_kind = XR_CHAN_GENERIC;
    ch->producer_coro_id = -1;
    ch->consumer_coro_id = -1;

    // Initialize state
    atomic_store_explicit(&ch->closed, false, memory_order_relaxed);
    xr_amutex_init(&ch->lock);

    // Timer specific fields
    atomic_store_explicit(&ch->is_timer, true, memory_order_relaxed);
    ch->timer_timeout_ms = timeout_ms;

    // Record start time
    ch->timer_start_ticks = xr_monotonic_ticks();

    atomic_store_explicit(&ch->timer_fired, false, memory_order_relaxed);

    // Initialize the embedded timer-wheel node.
    ch->tw_timer.prev = NULL;
    ch->tw_timer.next = NULL;
    atomic_init(&ch->tw_timer.cancel_next, NULL);
    ch->tw_timer.slot = XR_TW_SLOT_INACTIVE;
    atomic_init(&ch->tw_timer.state, XR_TIMER_STATE_ACTIVE);

    ch->elem_tid = 0;
    ch->dist = NULL;
    ch->name = NULL;
    ch->isolate = X;

    atomic_fetch_add(&xr_isolate_get_sys_heap(X)->stats.channel_create_count, 1);
    return ch;
}

// Arm the timer channel on the given timer wheel.
// Must be called from the owner worker after xr_channel_new_timer().
// If the timeout has already elapsed (e.g. after 0), fire immediately
// so that the first OP_CHAN_TRY_RECV poll finds data in the buffer.
void xr_channel_timer_arm(XrChannel *ch, XrTimerWheel *tw) {
    if (!ch || !tw)
        return;
    XR_DCHECK(atomic_load_explicit(&ch->is_timer, memory_order_relaxed),
              "xr_channel_timer_arm: not a timer channel");

    int64_t timeout_pos = ch->timer_start_ticks + ch->timer_timeout_ms;
    int64_t now = xr_monotonic_ticks();
    if (now >= timeout_pos) {
        // Already elapsed: fire callback inline (avoids round-trip via wheel)
        timer_channel_fire_cb(ch);
        return;
    }
    xr_twheel_set_timer(tw, &ch->tw_timer, timer_channel_fire_cb, ch, timeout_pos);
}

// Check if Timer Channel has fired (thin atomic check, no polling).
// Returns true if the timer has already delivered its value to the buffer.
bool xr_channel_timer_ready(XrChannel *ch) {
    if (!ch)
        return false;
    if (!atomic_load_explicit(&ch->is_timer, memory_order_relaxed)) {
        return false;
    }
    return atomic_load_explicit(&ch->timer_fired, memory_order_acquire);
}

void xr_channel_destroy(XrChannel *ch) {
    if (ch == NULL)
        return;

    // Notify cluster layer before destroying
    XrChannelDistHooks *hooks = ch->isolate ? ch->isolate->channel_dist_hooks : NULL;
    if (ch->dist && hooks && hooks->destroy) {
        hooks->destroy(ch);
        ch->dist = NULL;
    }

    // Only free buffer if separately allocated (not inline)
    if (ch->buffer != NULL && !channel_buffer_is_inline(ch)) {
        xr_free(ch->buffer);
        ch->buffer = NULL;
    }
}

// ========== Channel Send/Recv Primitives ==========
//
// These helpers factor the direct-transfer and buffer push/pop patterns
// that used to be copy-pasted across xr_channel_send / xr_channel_recv /
// xr_channel_try_send / xr_channel_try_recv / xr_channel_notify_send.
//
// Locking contract (applies to all four helpers):
//   - Caller MUST hold ch->lock on entry.
//   - chan_direct_{send,recv} release the lock and wake the peer on success.
//     On failure they touch nothing (lock still held).
//   - chan_buffer_{push,pop} leave the lock held either way; the caller is
//     responsible for unlocking once control flow converges.

// Forward declarations
static void channel_wake_coro(XrCoroutine *coro);
static void channel_wake_coro_ex(XrCoroutine *coro, bool is_close);

static void channel_wake_select_waiter(XrChannel *ch) {
    if (!ch || !ch->isolate)
        return;
    xr_runtime_wake_channel(ch->isolate, ch, false);
}

static void channel_wake_select_waiter_if_present(XrChannel *ch) {
    if (!ch)
        return;
    if (xr_channel_select_waiter_mask(ch) == 0) {
        XrRuntime *runtime = channel_stats_runtime(ch);
        if (runtime) {
            xr_sched_metric_inc(runtime, &runtime->sched_stats.chan_buffer_no_waiter_count);
        }
        return;
    }
    channel_wake_select_waiter(ch);
}

static void channel_refresh_any_waiter_mask(XrChannel *ch) {
    if (!ch)
        return;
    uint64_t mask = atomic_load_explicit(&ch->sender_waiter_worker_mask, memory_order_acquire) |
                    atomic_load_explicit(&ch->receiver_waiter_worker_mask, memory_order_acquire) |
                    atomic_load_explicit(&ch->select_waiter_worker_mask, memory_order_acquire);
    atomic_store_explicit(&ch->waiter_worker_mask, mask, memory_order_release);
}

static void channel_clear_drained_direction_masks(XrChannel *ch, bool send_drained,
                                                  bool recv_drained) {
    if (!ch)
        return;
    if (send_drained) {
        atomic_store_explicit(&ch->sender_waiter_worker_mask, 0, memory_order_release);
    }
    if (recv_drained) {
        atomic_store_explicit(&ch->receiver_waiter_worker_mask, 0, memory_order_release);
    }
    channel_refresh_any_waiter_mask(ch);
}

// Direct transfer: hand value to a blocked receiver and wake it.
// Returns true on success (lock released, wake dispatched).
static inline bool chan_direct_send(XrChannel *ch, XrValue v, XrCoroutine *producer) {
    XrCoroutine *receiver = xr_waitq_dequeue(&ch->recvq);
    if (!receiver)
        return false;
    CHANNEL_METRIC_INC(ch, chan_send_direct_count);
    CHANNEL_METRIC_INC(ch, chan_recvq_dequeue_count);
    channel_note_participant_locked(ch, producer, true);
    (void) xr_coro_store_recv_value(receiver, v);
    xr_amutex_unlock(&ch->lock);
    channel_wake_coro(receiver);
    return true;
}

// Direct transfer: pick up value from a blocked sender and wake it.
// Handles both unbuffered (take sender's value) and full-buffered (rotate
// buffer head out, push sender's value to tail) cases.
// Returns true on success (lock released, *out assigned).
static inline bool chan_direct_recv(XrChannel *ch, XrValue *out, XrCoroutine *consumer) {
    XrCoroutine *sender = xr_waitq_dequeue(&ch->sendq);
    if (!sender)
        return false;
    channel_note_participant_locked(ch, consumer, false);
    XrValue direct_val;
    if (ch->buf_size == 0) {
        direct_val = sender->ext->send_value;
    } else {
        if (ch->buf_count == 0) {
            waitq_enqueue_front(&ch->sendq, sender);
            return false;
        }
        direct_val = ch->buffer[ch->recv_idx];
        ch->recv_idx = chan_advance_idx(ch->recv_idx, ch->buf_size);
        ch->buffer[ch->send_idx] = sender->ext->send_value;
        ch->send_idx = chan_advance_idx(ch->send_idx, ch->buf_size);
        // buf_count unchanged: take one, put one.
    }
    CHANNEL_METRIC_INC(ch, chan_recv_direct_count);
    CHANNEL_METRIC_INC(ch, chan_sendq_dequeue_count);
    sender->ext->send_value = xr_null();
    xr_amutex_unlock(&ch->lock);
    *out = direct_val;
    channel_wake_coro(sender);
    return true;
}

// Buffer push: returns true if written. Lock remains held.
static inline bool chan_buffer_push(XrChannel *ch, XrValue v) {
    if (ch->buf_size == 0 || ch->buf_count >= ch->buf_size)
        return false;
    XR_DCHECK(ch->send_idx < ch->buf_size, "chan_buffer_push: send_idx OOR");
    ch->buffer[ch->send_idx] = v;
    ch->send_idx = chan_advance_idx(ch->send_idx, ch->buf_size);
    ch->buf_count++;
    XR_DCHECK(ch->buf_count <= ch->buf_size, "chan_buffer_push: overflow");
    CHANNEL_METRIC_INC(ch, chan_send_buffer_count);
    return true;
}

// Buffer pop: returns true if read. Lock remains held.
static inline bool chan_buffer_pop(XrChannel *ch, XrValue *out) {
    if (ch->buf_size == 0 || ch->buf_count == 0)
        return false;
    XR_DCHECK(ch->recv_idx < ch->buf_size, "chan_buffer_pop: recv_idx OOR");
    *out = ch->buffer[ch->recv_idx];
    ch->buffer[ch->recv_idx] = xr_null();  // Help GC.
    ch->recv_idx = chan_advance_idx(ch->recv_idx, ch->buf_size);
    ch->buf_count--;
    CHANNEL_METRIC_INC(ch, chan_recv_buffer_count);
    return true;
}

// ========== Non-blocking Send ==========

// Send value and wake any blocked receiver (for C code outside VM).
// Unlike try_send, this properly wakes receivers via channel_wake_coro
// which sets resume_status = XR_RESUME_CHANNEL.
bool xr_channel_notify_send(XrChannel *ch, XrValue value) {
    if (!ch)
        return false;

    xr_channel_lock_observed(ch);

    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_amutex_unlock(&ch->lock);
        return false;
    }

    // Direct transfer to a blocked receiver, or buffer push if space.
    if (chan_direct_send(ch, value, NULL))
        return true;
    if (chan_buffer_push(ch, value)) {
        channel_note_participant_locked(ch, NULL, true);
        xr_amutex_unlock(&ch->lock);
        channel_wake_select_waiter_if_present(ch);
        return true;
    }

    xr_amutex_unlock(&ch->lock);
    return false;
}

// Unbuffered: always returns false, VM handles sync via Worker queue
// Buffered: put into buffer
bool xr_channel_try_send(XrChannel *ch, XrValue value) {
    XR_DCHECK(ch != NULL, "channel is NULL");

    // Distributed channel: delegate to cluster hooks
    XrChannelDistHooks *hooks = ch->isolate ? ch->isolate->channel_dist_hooks : NULL;
    if (ch->dist && hooks && hooks->try_send) {
        return hooks->try_send(ch, value);
    }

    xr_channel_lock_observed(ch);

    // Check if closed
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_amutex_unlock(&ch->lock);
        return false;
    }

    // Direct transfer to waiting receiver (must be checked first, otherwise
    // blocked receivers are never woken by trySend).
    if (chan_direct_send(ch, value, NULL))
        return true;
    if (chan_buffer_push(ch, value)) {
        channel_note_participant_locked(ch, NULL, true);
        xr_amutex_unlock(&ch->lock);
        channel_wake_select_waiter_if_present(ch);
        return true;
    }

    // Unbuffered or buffer full, cannot send
    xr_amutex_unlock(&ch->lock);
    return false;
}

// ========== Non-blocking Receive ==========

// Unbuffered: always returns false, VM handles sync via Worker queue
// Buffered: take from buffer
XrValue xr_channel_try_recv(XrChannel *ch, bool *ok) {
    XR_DCHECK(ch != NULL, "channel is NULL");
    XR_DCHECK(ok != NULL, "ok pointer is NULL");

    // Distributed channel: delegate to cluster hooks
    XrChannelDistHooks *hooks = ch->isolate ? ch->isolate->channel_dist_hooks : NULL;
    if (ch->dist && hooks && hooks->try_recv) {
        return hooks->try_recv(ch, ok);
    }

    xr_channel_lock_observed(ch);

    // Direct transfer from waiting sender (handles unbuffered and full-
    // buffered rotate). Must be checked first, otherwise blocked senders
    // are never woken by tryRecv.
    XrValue value;
    if (chan_direct_recv(ch, &value, NULL)) {
        *ok = true;
        return value;
    }
    if (chan_buffer_pop(ch, &value)) {
        channel_note_participant_locked(ch, NULL, false);
        xr_amutex_unlock(&ch->lock);
        *ok = true;
        return value;
    }

    // Check if closed
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_amutex_unlock(&ch->lock);
        *ok = false;
        return xr_null();
    }

    // No data available
    xr_amutex_unlock(&ch->lock);
    *ok = false;
    return xr_null();
}

// ========== Channel Close ==========

#include "xyieldable.h"

// Internal: wake coroutine (set state and add to run queue)
// Uses affinity_p to determine target Worker, supports cross-Worker wake
// is_close: whether this is a close wake (needs to recheck buffer)
static XrRuntime *channel_wake_runtime(XrCoroutine *coro, XrWorker **out_current) {
    XrWorker *current = xr_current_worker();
    XrRuntime *coro_runtime =
        (coro && coro->isolate) ? (XrRuntime *) coro->isolate->vm.runtime : NULL;
    XrRuntime *runtime = NULL;
    if (current && current->p.runtime && (!coro_runtime || current->p.runtime == coro_runtime)) {
        runtime = current->p.runtime;
    } else {
        runtime = coro_runtime;
        current = NULL;
    }
    if (out_current)
        *out_current = current;
    return runtime;
}

static void channel_wake_coro_ex(XrCoroutine *coro, bool is_close) {
    XR_DCHECK(coro != NULL, "channel_wake_coro_ex: NULL coro");

    XrWorker *current = NULL;
    XrRuntime *runtime = channel_wake_runtime(coro, &current);
    if (!runtime || !runtime->workers || runtime->worker_count <= 0)
        return;

    // Atomically claim the wake. Only the caller that transitions the coro
    // BLOCKED -> READY proceeds; a racing waker (concurrent send/recv, close,
    // or timer fire) observes the coro is no longer BLOCKED and bails out.
    // This makes the resume-reason write and the enqueue below single-owner.
    if (!xr_coro_claim_wake(coro)) {
        return;
    }

    if (coro->ext)
        xr_channel_wait_token_resolve(&coro->ext->chan_wait_token);
    xr_coro_resume_store(coro, is_close ? XR_RESUME_CHANNEL_CLOSED : XR_RESUME_CHANNEL);
    atomic_store_explicit((_Atomic(void *) *) &coro->ext->wait_channel, NULL, memory_order_release);

    // Cancel timer (sendTimeout/recvTimeout case)
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        xr_timer_wait_token_cancel(&coro->ext->wait.timer_token);
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
    }

    xr_sched_metric_inc(runtime, is_close ? &runtime->sched_stats.chan_close_ready_wake_count
                                          : &runtime->sched_stats.chan_ready_wake_count);
    int target_id = xr_coro_wake_target_id(coro);

    // Ensure target Worker ID is valid
    if (target_id < 0 || target_id >= runtime->worker_count) {
        target_id = current ? current->p.id : 0;
    }

    bool in_owner_blocked_bucket = coro->ext->wait_bucket != NULL &&
                                   coro->ext->wait_bucket_owner >= 0 &&
                                   coro->ext->wait_bucket_owner < runtime->worker_count;
    if (in_owner_blocked_bucket) {
        target_id = coro->ext->wait_bucket_owner;
    }
    XrWorker *target = &runtime->workers[target_id];
    bool locked = xr_coro_is_thread_locked(coro);

    if (!current) {
        xr_worker_inbox_enqueue(runtime, target_id, coro);
        return;
    }

    if (in_owner_blocked_bucket && target == current) {
        xr_worker_unblock(current, coro);
    }

    // Thread-locked coroutines must return to their locked worker.
    // Route via inbox if the locked worker is not the current worker.
    if (locked && target != current) {
        xr_worker_inbox_enqueue(runtime, target_id, coro);
        return;
    }

    // Timed channel waits are mirrored in the owner worker's blocked bucket.
    // The owner must unlink that bucket node before the coroutine can be stolen
    // or recycled, so cross-worker timed wakes go through the owner inbox.
    if (in_owner_blocked_bucket && target != current) {
        xr_worker_inbox_enqueue(runtime, target_id, coro);
        return;
    }

    // Wake strategy for channel send/recv completions:
    //
    // Normal path (non-close wake): pull to current worker's LIFO for
    // maximum locality.  This is critical for pipeline patterns where
    // stages form serial chains (stage[i] recv -> wake stage[i-1],
    // stage[i] send -> wake stage[i+1]).  Cross-worker inbox delivery
    // adds 10-100x latency per hop, collapsing pipeline throughput with
    // many stages.
    //
    // Close fan-out: xr_channel_close may have hundreds of
    // waiters.  Piling them all onto the current worker's LIFO serializes
    // every subsequent wake on one thread.  For cross-worker waiters we
    // route via the target worker's MPSC inbox, so the fan-out parallelises
    // across workers.  Same-worker waiters still use LIFO for locality.
    if (is_close && target != current) {
        xr_worker_inbox_enqueue(runtime, target_id, coro);
        return;
    }
    if (target != current && !locked) {
        atomic_store_explicit(&coro->affinity_p, current->p.id, memory_order_relaxed);
    }
    xr_worker_push_lifo(current, coro);
}

// Normal wake (send/recv complete)
static void channel_wake_coro(XrCoroutine *coro) {
    channel_wake_coro_ex(coro, false);
}

static bool channel_close_defer_to_owner_bucket(XrCoroutine *coro, XrWorker *current) {
    if (!coro || !current || !current->p.runtime)
        return false;
    int owner = coro->ext->wait_bucket_owner;
    return coro->ext->wait_bucket != NULL && owner >= 0 &&
           owner < current->p.runtime->worker_count && owner != current->p.id;
}

// Close channel
// After close: send returns false, recv can still get buffered data
// When buffer empty, recv returns null + ok=false
// Wakes all waiting coroutines
void xr_channel_close(XrChannel *ch) {
    XR_DCHECK(ch != NULL, "channel is NULL");

    // Distributed channel: notify cluster before local close
    XrChannelDistHooks *hooks = ch->isolate ? ch->isolate->channel_dist_hooks : NULL;
    if (ch->dist && hooks && hooks->close) {
        hooks->close(ch);
    }

    xr_channel_lock_observed(ch);

    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        // Already closed, idempotent
        xr_amutex_unlock(&ch->lock);
        return;
    }

    atomic_store_explicit(&ch->closed, true, memory_order_release);
    if (ch->isolate && xr_isolate_get_sys_heap(ch->isolate)) {
        atomic_fetch_add(&xr_isolate_get_sys_heap(ch->isolate)->stats.channel_close_count, 1);
    }

    // Collect all waiters, wake after releasing lock
    XrCoroutine *recv_list = NULL;
    XrCoroutine *send_list = NULL;

    // Collect all waiting receivers
    // Note: don't set recv_slot here, let coroutine re-execute recv logic
    // This way if buffer still has data, coroutine can get it
    uint64_t recv_waiters = 0;
    uint64_t send_waiters = 0;
    XrCoroutine *coro;
    while ((coro = xr_waitq_dequeue(&ch->recvq)) != NULL) {
        coro->ext->chan_wait_next = recv_list;
        recv_list = coro;
        recv_waiters++;
    }

    // Collect all waiting senders
    while ((coro = xr_waitq_dequeue(&ch->sendq)) != NULL) {
        coro->ext->send_value = xr_null();
        coro->ext->chan_wait_next = send_list;
        send_list = coro;
        send_waiters++;
    }

    xr_amutex_unlock(&ch->lock);
    CHANNEL_METRIC_ADD(ch, chan_close_recv_waiter_count, recv_waiters);
    CHANNEL_METRIC_ADD(ch, chan_close_send_waiter_count, send_waiters);

    XrWorker *current = xr_current_worker();
    uint64_t deferred_recv_waiters = 0;
    uint64_t deferred_send_waiters = 0;

    // Wake all waiters after releasing lock (close wake, let them recheck buffer)
    while (recv_list) {
        coro = recv_list;
        recv_list = coro->ext->chan_wait_next;
        coro->ext->chan_wait_next = NULL;
        if (!channel_close_defer_to_owner_bucket(coro, current)) {
            channel_wake_coro_ex(coro, true);  // close wake
        } else {
            deferred_recv_waiters++;
        }
    }
    while (send_list) {
        coro = send_list;
        send_list = coro->ext->chan_wait_next;
        coro->ext->chan_wait_next = NULL;
        if (!channel_close_defer_to_owner_bucket(coro, current)) {
            channel_wake_coro_ex(coro, true);  // close wake
        } else {
            deferred_send_waiters++;
        }
    }

    channel_clear_drained_direction_masks(ch, deferred_send_waiters == 0,
                                          deferred_recv_waiters == 0);
    CHANNEL_METRIC_ADD(ch, chan_close_deferred_send_waiter_count, deferred_send_waiters);
    CHANNEL_METRIC_ADD(ch, chan_close_deferred_recv_waiter_count, deferred_recv_waiters);
    xr_runtime_wake_channel_all(ch->isolate, ch);
}

bool xr_channel_is_closed(XrChannel *ch) {
    XR_DCHECK(ch != NULL, "channel is NULL");
    return atomic_load_explicit(&ch->closed, memory_order_acquire);
}

// ========== Blocking Operations (atomic) ==========

// Blocking send
// 1. Lock
// 2. If recvq has waiter: direct transfer to receiver
// 3. If buffer has space: put in buffer
// 4. Otherwise block: join sendq
// Key: sender completes value transfer, receiver wakes with value ready
XrChanResult xr_channel_send(XrChannel *ch, XrValue value, XrCoroutine *coro) {
    XR_DCHECK(ch != NULL, "channel is NULL");

    // Distributed channel: delegate to cluster hooks
    XrChannelDistHooks *hooks = ch->isolate ? ch->isolate->channel_dist_hooks : NULL;
    if (ch->dist && hooks && hooks->send) {
        return (XrChanResult) hooks->send(ch, value, coro);
    }

    // Fast path: lock-free check closed (relaxed OK, rechecked under lock)
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        return XR_CHAN_CLOSED;
    }

    // Trylock fast path: for buffered channels with buffer space and no waiters.
    // Avoids spin contention under high concurrency (e.g. 20 coros on 1 channel).
    if (ch->buf_size > 0) {
        CHANNEL_METRIC_INC(ch, chan_buffer_fast_try_count);
        if (channel_trylock_observed(ch)) {
            if (!atomic_load_explicit(&ch->closed, memory_order_relaxed) && !ch->recvq.first &&
                chan_buffer_push(ch, value)) {
                CHANNEL_METRIC_INC(ch, chan_buffer_fast_hit_count);
                channel_note_participant_locked(ch, coro, true);
                xr_amutex_unlock(&ch->lock);
                channel_wake_select_waiter_if_present(ch);
                return XR_CHAN_OK;
            }
            CHANNEL_METRIC_INC(ch, chan_buffer_fast_miss_count);
            // Conditions not met under trylock: fall through with lock held
            goto send_locked;
        }
        CHANNEL_METRIC_INC(ch, chan_buffer_fast_busy_count);
        channel_lock_after_failed_try(ch);
    } else {
        xr_channel_lock_observed(ch);
    }

send_locked:
    // Recheck closed (with lock held)
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_amutex_unlock(&ch->lock);
        return XR_CHAN_CLOSED;
    }

    // Direct transfer to waiting receiver, or buffer push if space.
    if (chan_direct_send(ch, value, coro))
        return XR_CHAN_OK;
    if (chan_buffer_push(ch, value)) {
        channel_note_participant_locked(ch, coro, true);
        xr_amutex_unlock(&ch->lock);
        channel_wake_select_waiter_if_present(ch);
        return XR_CHAN_OK;
    }

    // Case 3: need to block
    if (!coro) {
        xr_amutex_unlock(&ch->lock);
        return XR_CHAN_NO_CORO;
    }
    if (!xr_coro_ensure_ext(coro)) {
        xr_amutex_unlock(&ch->lock);
        return XR_CHAN_FULL;
    }

    // Set blocked state and join sendq
    xr_channel_wait_token_prepare(&coro->ext->chan_wait_token, ch, true);
    channel_note_participant_locked(ch, coro, true);
    atomic_store_explicit(&coro->ext->wait_channel, ch, memory_order_release);
    coro->ext->wait_send = true;
    coro->ext->send_value = value;  // Save value to send
    (void) xr_coro_publish_locked_block(coro);
    // Set affinity_p for cross-Worker wake + waiter mask for routing
    XrWorker *w = xr_current_worker();
    if (w) {
        atomic_store_explicit(&coro->affinity_p, w->p.id, memory_order_relaxed);
        xr_channel_note_waiter(ch, w->p.id, true);
    }
    xr_waitq_enqueue(&ch->sendq, coro);
    xr_channel_wait_token_commit(&coro->ext->chan_wait_token);
    CHANNEL_METRIC_INC(ch, chan_send_block_count);

    xr_amutex_unlock(&ch->lock);
    return XR_CHAN_BLOCK;
}

// Blocking recv
// 1. Lock
// 2. If sendq has waiter:
//    - Unbuffered: take value directly from sender
//    - Buffered: take from buffer head, sender's value goes to buffer tail
// 3. If buffer has data: take from buffer
// 4. Otherwise block: join recvq
// Key: receiver completes value transfer, sender wakes with value taken
XrChanResult xr_channel_recv(XrChannel *ch, XrValue *out, XrCoroutine *coro) {
    XR_DCHECK(ch != NULL, "channel is NULL");
    XR_DCHECK(out != NULL, "out pointer is NULL");

    // Distributed channel: delegate to cluster hooks
    XrChannelDistHooks *hooks = ch->isolate ? ch->isolate->channel_dist_hooks : NULL;
    if (ch->dist && hooks && hooks->recv) {
        return (XrChanResult) hooks->recv(ch, out, coro);
    }

    // Trylock fast path: for buffered channels with data and no waiting senders.
    // Avoids spin contention under high concurrency.
    if (ch->buf_size > 0) {
        CHANNEL_METRIC_INC(ch, chan_buffer_fast_try_count);
        if (channel_trylock_observed(ch)) {
            if (!ch->sendq.first && chan_buffer_pop(ch, out)) {
                CHANNEL_METRIC_INC(ch, chan_buffer_fast_hit_count);
                channel_note_participant_locked(ch, coro, false);
                xr_amutex_unlock(&ch->lock);
                return XR_CHAN_OK;
            }
            CHANNEL_METRIC_INC(ch, chan_buffer_fast_miss_count);
            // Conditions not met under trylock: fall through with lock held
            goto recv_locked;
        }
        CHANNEL_METRIC_INC(ch, chan_buffer_fast_busy_count);
        channel_lock_after_failed_try(ch);
    } else {
        xr_channel_lock_observed(ch);
    }

recv_locked:
    // Direct transfer from waiting sender, or buffer pop.
    if (chan_direct_recv(ch, out, coro))
        return XR_CHAN_OK;
    if (chan_buffer_pop(ch, out)) {
        channel_note_participant_locked(ch, coro, false);
        xr_amutex_unlock(&ch->lock);
        return XR_CHAN_OK;
    }

    // Case 3: channel closed and buffer empty
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_amutex_unlock(&ch->lock);
        *out = xr_null();
        return XR_CHAN_CLOSED;
    }

    // Case 4: need to block
    if (!coro) {
        xr_amutex_unlock(&ch->lock);
        return XR_CHAN_NO_CORO;
    }
    if (!xr_coro_ensure_ext(coro)) {
        xr_amutex_unlock(&ch->lock);
        return XR_CHAN_FULL;
    }

    // Set blocked state and join recvq
    xr_channel_wait_token_prepare(&coro->ext->chan_wait_token, ch, false);
    channel_note_participant_locked(ch, coro, false);
    atomic_store_explicit(&coro->ext->wait_channel, ch, memory_order_release);
    coro->ext->wait_send = false;
    (void) xr_coro_publish_locked_block(coro);
    // Set affinity_p for cross-Worker wake + waiter mask for routing
    XrWorker *w = xr_current_worker();
    if (w) {
        atomic_store_explicit(&coro->affinity_p, w->p.id, memory_order_relaxed);
        xr_channel_note_waiter(ch, w->p.id, false);
    }
    xr_waitq_enqueue(&ch->recvq, coro);
    xr_channel_wait_token_commit(&coro->ext->chan_wait_token);
    CHANNEL_METRIC_INC(ch, chan_recv_block_count);

    xr_amutex_unlock(&ch->lock);
    return XR_CHAN_BLOCK;
}

#undef CHANNEL_METRIC_ADD
#undef CHANNEL_METRIC_INC
