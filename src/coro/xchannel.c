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
#include "xcoroutine.h"
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

// Ring buffer index advance: conditional increment is faster than modulo division
// on ARM64 (~1-2 cycles vs ~10 cycles for SDIV+MSUB)
static inline uint32_t chan_advance_idx(uint32_t idx, uint32_t buf_size) {
    return (++idx >= buf_size) ? 0 : idx;
}

// Distributed channel hooks live on XrayIsolate::channel_dist_hooks.
// Fetch via channel->isolate->channel_dist_hooks in the functions below.
//
// Channel close counter lives on XrSystemHeap::stats.channel_close_count
// (mirrors channel_create_count; see xsystem_heap.h). Access via the
// isolate-aware xr_channel_get_close_count(X) below.

uint64_t xr_channel_get_close_count(struct XrayIsolate *X) {
    if (!X || !xr_isolate_get_sys_heap(X)) return 0;
    return atomic_load(&xr_isolate_get_sys_heap(X)->stats.channel_close_count);
}

// ========== Wait Queue Implementation ==========

// Uses coroutine's wait_link field (not sched_link!) as queue link.
// sched_link is reserved for MPSC inbox / run queue to avoid collision.
// Coroutine must be in BLOCKED state when enqueued.

void xr_waitq_enqueue(XrWaitQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "waitq_enqueue: NULL queue");
    XR_DCHECK(coro != NULL, "waitq_enqueue: NULL coro");
    coro->wait_link = NULL;
    if (q->last) {
        q->last->wait_link = coro;
    } else {
        q->first = coro;
    }
    q->last = coro;
}

XrCoroutine *xr_waitq_dequeue(XrWaitQueue *q) {
    XR_DCHECK(q != NULL, "waitq_dequeue: NULL queue");
    XrCoroutine *coro = q->first;
    if (coro) {
        q->first = coro->wait_link;
        if (q->first == NULL) {
            q->last = NULL;
        }
        coro->wait_link = NULL;
    }
    return coro;
}

// Remove specific coroutine from wait queue (for timeout cancellation)
static bool xr_waitq_remove(XrWaitQueue *q, XrCoroutine *coro) {
    XR_DCHECK(q != NULL, "waitq_remove: NULL queue");
    XR_DCHECK(coro != NULL, "waitq_remove: NULL coro");
    XrCoroutine *prev = NULL;
    XrCoroutine *curr = q->first;

    while (curr) {
        if (curr == coro) {
            // Found, remove
            if (prev) {
                prev->wait_link = curr->wait_link;
            } else {
                q->first = curr->wait_link;
            }
            if (curr == q->last) {
                q->last = prev;
            }
            curr->wait_link = NULL;
            return true;
        }
        prev = curr;
        curr = curr->wait_link;
    }
    return false;
}

// Remove coroutine from channel wait queue (called on timeout)
void xr_channel_remove_waiter(XrChannel *ch, XrCoroutine *coro) {
    if (!ch || !coro) return;

    xr_mutex_lock(&ch->lock);

    // Try to remove from send queue
    if (!xr_waitq_remove(&ch->sendq, coro)) {
        // Not in send queue, try recv queue
        xr_waitq_remove(&ch->recvq, coro);
    }

    xr_mutex_unlock(&ch->lock);
}

// ========== GC Callbacks ==========

// Check if buffer is inline (allocated together with channel)
static inline bool channel_buffer_is_inline(XrChannel *ch) {
    return ch->buffer == (XrValue*)(ch + 1);
}

// GC destroy: free buffer only if separately allocated
void xr_gc_destroy_channel(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void)owning_gc;
    XrChannel *ch = (XrChannel *)obj;
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
    if (!X || !xr_isolate_get_sys_heap(X)) return NULL;

    // Single allocation: XrChannel + inline buffer (like Go's makechan)
    size_t alloc_size = sizeof(XrChannel) + (size_t)buffer_size * sizeof(XrValue);
    XrChannel *ch = (XrChannel *)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), alloc_size, XR_TCHANNEL);
    if (!ch) return NULL;

    // Set initial refcount to 1
    xr_shared_set_refc(&ch->gc_header, 1);

    // xr_sysheap_alloc_shared already memset(0) the entire allocation.
    // All fields default to 0/NULL/false which is correct for:
    //   buffer(NULL), buf_size(0), buf_count(0), send_idx(0), recv_idx(0),
    //   sendq(NULL,NULL), recvq(NULL,NULL), closed(0), lock(UNLOCKED=0),
    //   is_timer(0), timer_*(0), elem_tid(0), dist(NULL), name(NULL).
    // Only set non-zero fields.
    if (buffer_size > 0) {
        ch->buffer = (XrValue*)(ch + 1);
        ch->buf_size = buffer_size;
    }
    ch->isolate = X;

    atomic_fetch_add(&xr_isolate_get_sys_heap(X)->stats.channel_create_count, 1);
    return ch;
}

// Timer wheel callback for time.after.
// Fires once when the timeout elapses, writes current time to the channel
// buffer so that any subsequent recv/tryRecv finds data immediately.
// If a receiver is already blocked on this channel, wake it directly.
static void timer_channel_fire_cb(void *arg) {
    XrChannel *ch = (XrChannel *)arg;
    XR_DCHECK(ch != NULL, "timer_channel_fire_cb: NULL channel");
    if (!ch) return;

    // Double-check: callback should only fire once
    if (atomic_load_explicit(&ch->timer_fired, memory_order_relaxed)) return;

    int64_t now = xr_monotonic_ticks();
    XrCoroutine *receiver = NULL;

    xr_mutex_lock(&ch->lock);
    if (!atomic_load_explicit(&ch->timer_fired, memory_order_relaxed)) {
        // Try to hand value directly to a blocked receiver
        receiver = xr_waitq_dequeue(&ch->recvq);
        if (receiver) {
            // Direct transfer to blocked receiver's slot
            XrValue *slot = receiver->recv_slot;
            if (slot) *slot = xr_int(now);
        } else {
            // No receiver waiting: leave value in buffer for later recv
            ch->buffer[0] = xr_int(now);
            ch->buf_count = 1;
        }
        atomic_store_explicit(&ch->timer_fired, true, memory_order_release);
    }
    xr_mutex_unlock(&ch->lock);

    // Wake receiver outside lock (same pattern as chan_direct_send)
    if (receiver) {
        channel_wake_coro(receiver);
    }
}

// Create Timer Channel
// Returns a read-only channel that sends current time after timeout.
// Uses the channel's embedded tw_timer for timer wheel registration.
XrChannel *xr_channel_new_timer(struct XrayIsolate *X, int64_t timeout_ms) {
    if (!X || !xr_isolate_get_sys_heap(X)) return NULL;

    // Single allocation: XrChannel + 1-element inline buffer
    size_t alloc_size = sizeof(XrChannel) + sizeof(XrValue);
    XrChannel *ch = (XrChannel *)xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), alloc_size, XR_TCHANNEL);
    if (!ch) return NULL;

    // Set initial refcount to 1
    xr_shared_set_refc(&ch->gc_header, 1);

    // Inline single-element buffer
    ch->buffer = (XrValue*)(ch + 1);
    ch->buffer[0] = xr_null();
    ch->buf_size = 1;
    ch->buf_count = 0;
    ch->send_idx = 0;
    ch->recv_idx = 0;

    // Initialize state
    atomic_store_explicit(&ch->closed, false, memory_order_relaxed);
    xr_mutex_init(&ch->lock);

    // Timer specific fields
    atomic_store_explicit(&ch->is_timer, true, memory_order_relaxed);
    ch->timer_timeout_ms = timeout_ms;

    // Record start time
    ch->timer_start_ticks = xr_monotonic_ticks();

    atomic_store_explicit(&ch->timer_fired, false, memory_order_relaxed);

    // Initialize the embedded timer-wheel node.
    ch->tw_timer.prev = NULL;
    ch->tw_timer.next = NULL;
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
    if (!ch || !tw) return;
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
    if (!ch) return false;
    if (!atomic_load_explicit(&ch->is_timer, memory_order_relaxed)) {
        return false;
    }
    return atomic_load_explicit(&ch->timer_fired, memory_order_acquire);
}

void xr_channel_destroy(XrChannel *ch) {
    if (ch == NULL) return;

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

// ========== Fast Path Checks (Lock-free) ==========

// Check if channel is full (only checks buffer)
static inline bool channel_full(XrChannel *ch) {
    if (ch->buf_size == 0) {
        // Unbuffered: always "full", VM handles sync
        return true;
    }
    return ch->buf_count == ch->buf_size;
}

// Check if channel is empty (only checks buffer)
static inline bool channel_empty(XrChannel *ch) {
    if (ch->buf_size == 0) {
        // Unbuffered: always "empty", VM handles sync
        return true;
    }
    return ch->buf_count == 0;
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

// Direct transfer: hand value to a blocked receiver and wake it.
// Returns true on success (lock released, wake dispatched).
static inline bool chan_direct_send(XrChannel *ch, XrValue v) {
    XrCoroutine *receiver = xr_waitq_dequeue(&ch->recvq);
    if (!receiver) return false;
    XrValue *slot = receiver->recv_slot;
    if (slot) *slot = v;
    xr_mutex_unlock(&ch->lock);
    channel_wake_coro(receiver);
    return true;
}

// Direct transfer: pick up value from a blocked sender and wake it.
// Handles both unbuffered (take sender's value) and full-buffered (rotate
// buffer head out, push sender's value to tail) cases.
// Returns true on success (lock released, *out assigned).
static inline bool chan_direct_recv(XrChannel *ch, XrValue *out) {
    XrCoroutine *sender = xr_waitq_dequeue(&ch->sendq);
    if (!sender) return false;
    XrValue direct_val;
    if (ch->buf_size == 0) {
        direct_val = sender->send_value;
    } else {
        direct_val = ch->buffer[ch->recv_idx];
        ch->recv_idx = chan_advance_idx(ch->recv_idx, ch->buf_size);
        ch->buffer[ch->send_idx] = sender->send_value;
        ch->send_idx = chan_advance_idx(ch->send_idx, ch->buf_size);
        // buf_count unchanged: take one, put one.
    }
    sender->send_value = xr_null();
    xr_mutex_unlock(&ch->lock);
    *out = direct_val;
    channel_wake_coro(sender);
    return true;
}

// Buffer push: returns true if written. Lock remains held.
static inline bool chan_buffer_push(XrChannel *ch, XrValue v) {
    if (ch->buf_size == 0 || ch->buf_count >= ch->buf_size) return false;
    XR_DCHECK(ch->send_idx < ch->buf_size, "chan_buffer_push: send_idx OOR");
    ch->buffer[ch->send_idx] = v;
    ch->send_idx = chan_advance_idx(ch->send_idx, ch->buf_size);
    ch->buf_count++;
    XR_DCHECK(ch->buf_count <= ch->buf_size, "chan_buffer_push: overflow");
    return true;
}

// Buffer pop: returns true if read. Lock remains held.
static inline bool chan_buffer_pop(XrChannel *ch, XrValue *out) {
    if (ch->buf_size == 0 || ch->buf_count == 0) return false;
    XR_DCHECK(ch->recv_idx < ch->buf_size, "chan_buffer_pop: recv_idx OOR");
    *out = ch->buffer[ch->recv_idx];
    ch->buffer[ch->recv_idx] = xr_null();  // Help GC.
    ch->recv_idx = chan_advance_idx(ch->recv_idx, ch->buf_size);
    ch->buf_count--;
    return true;
}

// ========== Non-blocking Send ==========

// Send value and wake any blocked receiver (for C code outside VM).
// Unlike try_send, this properly wakes receivers via channel_wake_coro
// which sets resume_status = XR_RESUME_CHANNEL.
bool xr_channel_notify_send(XrChannel *ch, XrValue value) {
    if (!ch) return false;

    xr_mutex_lock(&ch->lock);

    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_mutex_unlock(&ch->lock);
        return false;
    }

    // Direct transfer to a blocked receiver, or buffer push if space.
    if (chan_direct_send(ch, value)) return true;
    if (chan_buffer_push(ch, value)) {
        xr_mutex_unlock(&ch->lock);
        return true;
    }

    xr_mutex_unlock(&ch->lock);
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

    // Fast path: lock-free check
    if (!atomic_load_explicit(&ch->closed, memory_order_relaxed) && channel_full(ch)) {
        return false;
    }

    xr_mutex_lock(&ch->lock);

    // Check if closed
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_mutex_unlock(&ch->lock);
        return false;
    }

    // Direct transfer to waiting receiver (must be checked first, otherwise
    // blocked receivers are never woken by trySend).
    if (chan_direct_send(ch, value)) return true;
    if (chan_buffer_push(ch, value)) {
        xr_mutex_unlock(&ch->lock);
        return true;
    }

    // Unbuffered or buffer full, cannot send
    xr_mutex_unlock(&ch->lock);
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

    // Fast path: lock-free check
    if (!atomic_load_explicit(&ch->closed, memory_order_relaxed) && channel_empty(ch)) {
        *ok = false;
        return xr_null();
    }

    xr_mutex_lock(&ch->lock);

    // Direct transfer from waiting sender (handles unbuffered and full-
    // buffered rotate). Must be checked first, otherwise blocked senders
    // are never woken by tryRecv.
    XrValue value;
    if (chan_direct_recv(ch, &value)) {
        *ok = true;
        return value;
    }
    if (chan_buffer_pop(ch, &value)) {
        xr_mutex_unlock(&ch->lock);
        *ok = true;
        return value;
    }

    // Check if closed
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_mutex_unlock(&ch->lock);
        *ok = false;
        return xr_null();
    }

    // No data available
    xr_mutex_unlock(&ch->lock);
    *ok = false;
    return xr_null();
}

// ========== Channel Close ==========

#include "xworker.h"
#include "xyieldable.h"

// Internal: wake coroutine (set state and add to run queue)
// Uses affinity_p to determine target Worker, supports cross-Worker wake
// is_close: whether this is a close wake (needs to recheck buffer)
static void channel_wake_coro_ex(XrCoroutine *coro, bool is_close) {
    XR_DCHECK(coro != NULL, "channel_wake_coro_ex: NULL coro");
    // Check if coroutine is done (avoid waking completed coroutine)
    if (xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        return;
    }

    xr_coro_resume_store(coro, is_close ? XR_RESUME_CHANNEL_CLOSED : XR_RESUME_CHANNEL);
    coro->wait_channel = NULL;  // Clear blocked state

    // Cancel timer (sendTimeout/recvTimeout case)
    if (coro->ext && atomic_load_explicit(&coro->ext->timer_active, memory_order_relaxed)) {
        atomic_store_explicit(&coro->ext->timer_active, false, memory_order_relaxed);
    }

    // Combine clear BLOCKED + set READY in one CAS (hot path optimization)
    xr_coro_transition_wake(coro);

    XrWorker *current = xr_current_worker();
    if (!current || !current->p.runtime) return;

    XrRuntime *runtime = current->p.runtime;
    int target_id = atomic_load_explicit(&coro->affinity_p, memory_order_relaxed);

    // Ensure target Worker ID is valid
    if (target_id < 0 || target_id >= runtime->worker_count) {
        target_id = current->p.id;
    }

    XrWorker *target = &runtime->workers[target_id];

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
    if (target != current) {
        atomic_store_explicit(&coro->affinity_p, current->p.id, memory_order_relaxed);
    }
    xr_worker_push_lifo(current, coro);
}

// Normal wake (send/recv complete)
static void channel_wake_coro(XrCoroutine *coro) {
    channel_wake_coro_ex(coro, false);
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

    xr_mutex_lock(&ch->lock);

    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        // Already closed, idempotent
        xr_mutex_unlock(&ch->lock);
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
    XrCoroutine *coro;
    while ((coro = xr_waitq_dequeue(&ch->recvq)) != NULL) {
        coro->wait_link = recv_list;
        recv_list = coro;
    }

    // Collect all waiting senders
    while ((coro = xr_waitq_dequeue(&ch->sendq)) != NULL) {
        coro->send_value = xr_null();
        coro->wait_link = send_list;
        send_list = coro;
    }

    xr_mutex_unlock(&ch->lock);

    // Wake all waiters after releasing lock (close wake, let them recheck buffer)
    while (recv_list) {
        coro = recv_list;
        recv_list = coro->wait_link;
        coro->wait_link = NULL;
        channel_wake_coro_ex(coro, true);  // close wake
    }
    while (send_list) {
        coro = send_list;
        send_list = coro->wait_link;
        coro->wait_link = NULL;
        channel_wake_coro_ex(coro, true);  // close wake
    }
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
        return (XrChanResult)hooks->send(ch, value, coro);
    }

    // Fast path: lock-free check closed (relaxed OK, rechecked under lock)
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        return XR_CHAN_CLOSED;
    }

    // Trylock fast path: for buffered channels with buffer space and no waiters.
    // Avoids spin contention under high concurrency (e.g. 20 coros on 1 channel).
    if (ch->buf_size > 0 && xr_mutex_trylock(&ch->lock)) {
        if (!atomic_load_explicit(&ch->closed, memory_order_relaxed) &&
            !ch->recvq.first && ch->buf_count < ch->buf_size) {
            ch->buffer[ch->send_idx] = value;
            ch->send_idx = chan_advance_idx(ch->send_idx, ch->buf_size);
            ch->buf_count++;
            xr_mutex_unlock(&ch->lock);
            return XR_CHAN_OK;
        }
        // Conditions not met under trylock: fall through with lock held
        goto send_locked;
    }

    xr_mutex_lock(&ch->lock);

send_locked:
    // Recheck closed (with lock held)
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_mutex_unlock(&ch->lock);
        return XR_CHAN_CLOSED;
    }

    // Direct transfer to waiting receiver, or buffer push if space.
    if (chan_direct_send(ch, value)) return XR_CHAN_OK;
    if (chan_buffer_push(ch, value)) {
        xr_mutex_unlock(&ch->lock);
        return XR_CHAN_OK;
    }

    // Case 3: need to block
    if (!coro) {
        xr_mutex_unlock(&ch->lock);
        return XR_CHAN_NO_CORO;
    }

    // Set blocked state and join sendq
    coro->wait_channel = ch;
    coro->wait_send = true;
    coro->send_value = value;  // Save value to send
    // caller pre-saved frame state before this call,
    // so it is safe to set BLOCKED here under the channel lock.  The coro
    // becomes wakeable only after we enqueue + unlock below.
    xr_coro_transition_to_blocked(coro);
    // Set affinity_p for cross-Worker wake + waiter mask for routing
    XrWorker *w = xr_current_worker();
    if (w) {
        atomic_store_explicit(&coro->affinity_p, w->p.id, memory_order_relaxed);
        atomic_fetch_or_explicit(&ch->waiter_worker_mask,
                                 (uint64_t)1 << w->p.id, memory_order_relaxed);
    }
    xr_waitq_enqueue(&ch->sendq, coro);

    xr_mutex_unlock(&ch->lock);
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
        return (XrChanResult)hooks->recv(ch, out, coro);
    }

    // Trylock fast path: for buffered channels with data and no waiting senders.
    // Avoids spin contention under high concurrency.
    if (ch->buf_size > 0 && xr_mutex_trylock(&ch->lock)) {
        if (!ch->sendq.first && ch->buf_count > 0) {
            *out = ch->buffer[ch->recv_idx];
            ch->buffer[ch->recv_idx] = xr_null();
            ch->recv_idx = chan_advance_idx(ch->recv_idx, ch->buf_size);
            ch->buf_count--;
            xr_mutex_unlock(&ch->lock);
            return XR_CHAN_OK;
        }
        // Conditions not met under trylock: fall through with lock held
        goto recv_locked;
    }

    xr_mutex_lock(&ch->lock);

recv_locked:
    // Direct transfer from waiting sender, or buffer pop.
    if (chan_direct_recv(ch, out)) return XR_CHAN_OK;
    if (chan_buffer_pop(ch, out)) {
        xr_mutex_unlock(&ch->lock);
        return XR_CHAN_OK;
    }

    // Case 3: channel closed and buffer empty
    if (atomic_load_explicit(&ch->closed, memory_order_relaxed)) {
        xr_mutex_unlock(&ch->lock);
        *out = xr_null();
        return XR_CHAN_CLOSED;
    }

    // Case 4: need to block
    if (!coro) {
        xr_mutex_unlock(&ch->lock);
        return XR_CHAN_NO_CORO;
    }

    // Set blocked state and join recvq
    coro->wait_channel = ch;
    coro->wait_send = false;
    // recv_slot already set by VM to stack register address
    // see send path comment for rationale.
    xr_coro_transition_to_blocked(coro);
    // Set affinity_p for cross-Worker wake + waiter mask for routing
    XrWorker *w = xr_current_worker();
    if (w) {
        atomic_store_explicit(&coro->affinity_p, w->p.id, memory_order_relaxed);
        atomic_fetch_or_explicit(&ch->waiter_worker_mask,
                                 (uint64_t)1 << w->p.id, memory_order_relaxed);
    }
    xr_waitq_enqueue(&ch->recvq, coro);

    xr_mutex_unlock(&ch->lock);
    return XR_CHAN_BLOCK;
}
