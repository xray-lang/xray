/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xchannel.h - Channel for inter-coroutine communication
 *
 * KEY CONCEPT:
 *   - Buffered and unbuffered modes
 *   - Wait queues inside channel (atomic operations)
 *   - Timer channel support for select timeout
 *
 * CHANNEL INVARIANTS:
 *
 *   INVARIANT 1 (Buffer ring): For buffered channels, send_idx and
 *   recv_idx advance modulo buf_size. buf_count tracks the number of
 *   items in the buffer: 0 <= buf_count <= buf_size. A send blocks
 *   when buf_count == buf_size; a recv blocks when buf_count == 0.
 *
 *   INVARIANT 2 (Unbuffered rendezvous): For unbuffered channels
 *   (buf_size == 0, buffer == NULL), send and recv must pair up.
 *   A sender blocks until a receiver arrives (or vice versa).
 *   The value is transferred directly from sender to receiver
 *   without intermediate storage.
 *
 *   INVARIANT 3 (Wait queue consistency): Under the channel lock,
 *   sendq and recvq each form a valid doubly-linked list via
 *   chan_wait_next / chan_wait_prev pointers. A coroutine is on at most
 *   one channel wait queue at a time. Enqueue/dequeue are always performed
 *   under the lock.
 *
 *   INVARIANT 4 (Close semantics): Once closed is set to true, no
 *   further sends are allowed (return XR_CHAN_CLOSED). Pending
 *   receivers are woken with null. Pending senders are woken with
 *   XR_CHAN_CLOSED. Buffered items remain readable after close
 *   until the buffer is drained.
 *
 *   INVARIANT 5 (Lock discipline): All mutations to buffer state
 *   (buf_count, send_idx, recv_idx) and wait queues (sendq, recvq)
 *   must be performed under the channel mutex (XrAdaptiveMutex). The lock
 *   is held for short durations only (no blocking operations under
 *   lock). XrAdaptiveMutex is an adaptive 3-state lock (active spin -> yield
 *   -> futex sleep) and degrades to a single CAS under no contention.
 *
 */

#ifndef XCHANNEL_H
#define XCHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "../runtime/value/xvalue.h"
#include "../runtime/mem/xobj_header.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "xslot_ref.h"
#include "xtimer_wheel.h"  // XrTWheelTimer (embedded in channel for timer wheel)

/* ========== Forward Declarations ========== */

typedef struct XrCoroutine XrCoroutine;
typedef struct XrCoroState XrCoroState;
struct XrVMRuntime;
struct XrRuntime;
struct XrRuntimeCore;

/* ========== Channel Lock ========== */

#include "../base/xmutex.h"

/* ========== Wait Queue ========== */

struct XrWaitQueue {
    XrCoroutine *first;
    XrCoroutine *last;
};

typedef struct XrWaitQueue XrWaitQueue;

static inline void xr_waitq_init(XrWaitQueue *q) {
    q->first = NULL;
    q->last = NULL;
}

static inline bool xr_waitq_is_empty(XrWaitQueue *q) {
    return q->first == NULL;
}

XR_FUNC void xr_waitq_enqueue(XrWaitQueue *q, XrCoroutine *coro);
XR_FUNC XrCoroutine *xr_waitq_dequeue(XrWaitQueue *q);
/* Deadlock-detector census: coroutines currently parked on any channel waitq. */
XR_FUNC int64_t xr_channel_waiters_total(void);

struct XrChannel;
// Remove coro from the channel's wait queue under ch->lock.
// Returns true when THIS call unlinked the coroutine — the caller now owns
// the wake decision (Go sudog semantics: dequeue under lock = exclusive
// ownership). Returns false when the coroutine was no longer queued, i.e.
// a concurrent send/recv/close already dequeued and owns it.
XR_FUNC bool xr_channel_remove_waiter(struct XrChannel *ch, XrCoroutine *coro);
XR_FUNC void xr_channel_lock_observed(struct XrChannel *ch);

/* ========== Channel Structure ========== */

typedef enum {
    XR_CHAN_GENERIC,
    XR_CHAN_SPSC,
    XR_CHAN_MPSC,
    XR_CHAN_MPMC,
    XR_CHAN_RENDEZVOUS,
    XR_CHAN_WORK_QUEUE
} XrChannelKind;

typedef struct XrChannel {
    XrObjHeader gc_header;

    /* === Buffer (ring buffer for buffered channels) === */
    XrValue *buffer;     // NULL for unbuffered
    uint32_t buf_size;   // 0 for unbuffered
    uint32_t buf_count;  // Current item count
    uint32_t send_idx;   // Next write position
    uint32_t recv_idx;   // Next read position
    _Atomic int kind;    // Runtime specialization hint, never part of user semantics
    _Atomic int worker_kind;
    uint64_t producer_worker_mask;
    uint64_t consumer_worker_mask;
    int producer_coro_id;
    int consumer_coro_id;
    bool producer_coro_multi;
    bool consumer_coro_multi;

    /* === Wait Queues === */
    XrWaitQueue sendq;  // Blocked senders
    XrWaitQueue recvq;  // Blocked receivers

    /* === State (atomic) === */
    _Atomic(bool) closed;
    XrAdaptiveMutex lock;

    /* === Timer Channel === */
    _Atomic(bool) is_timer;
    int64_t timer_timeout_ms;
    int64_t timer_start_ticks;
    _Atomic(bool) timer_fired;
    _Atomic(bool) timer_disposed;   // one-shot latch; dispose runs its release path exactly once
    struct XrTWheelTimer tw_timer;  // Embedded timer wheel node (avoids polling).
    uint8_t elem_tid;               // XrTypeId: element type for reified generics (0=any)

    /* === Waiter Worker Masks (ownership-safe wake routing) ===
     * Bit i in waiter_worker_mask is set when worker i has at least one
     * coroutine blocked on this channel.  Directional masks are hints used to
     * avoid waking a worker that only has waiters for the opposite operation.
     *
     * sender_waiter_worker_mask: workers with blocked senders.
     * receiver_waiter_worker_mask: workers with blocked receivers.
     * select_waiter_worker_mask: workers with select cases on this channel.
     *
     * Bits are set under ch->lock when a coroutine blocks; cleared lazily
     * by the owning worker after wake_one/wake_all finds no more waiters.
     * False positives (stale set bit) are harmless; false negatives are
     * prevented by always setting under lock before the coro is visible to
     * the wake path.  Supports up to 64 workers (XR_MAX_WORKERS). */
    _Atomic(uint64_t) waiter_worker_mask;
    _Atomic(uint64_t) sender_waiter_worker_mask;
    _Atomic(uint64_t) receiver_waiter_worker_mask;
    _Atomic(uint64_t) select_waiter_worker_mask;

    /* === Runtime owner === */
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;

    /* Optional VM host binding for cross-backend channel operations. */
    struct XrVMRuntime *vm_host_isolate;
} XrChannel;

static inline uint64_t xr_channel_worker_bit(int worker_id) {
    if (worker_id < 0 || worker_id >= 64)
        return 0;
    return (uint64_t) 1ull << worker_id;
}

static inline void xr_channel_note_waiter(XrChannel *ch, int worker_id, bool wait_send) {
    uint64_t bit = xr_channel_worker_bit(worker_id);
    if (!ch || bit == 0)
        return;

    atomic_fetch_or_explicit(&ch->waiter_worker_mask, bit, memory_order_release);
    _Atomic(uint64_t) *dir_mask =
        wait_send ? &ch->sender_waiter_worker_mask : &ch->receiver_waiter_worker_mask;
    atomic_fetch_or_explicit(dir_mask, bit, memory_order_release);
}

static inline void xr_channel_note_select_waiter(XrChannel *ch, int worker_id) {
    uint64_t bit = xr_channel_worker_bit(worker_id);
    if (!ch || bit == 0)
        return;

    atomic_fetch_or_explicit(&ch->waiter_worker_mask, bit, memory_order_release);
    atomic_fetch_or_explicit(&ch->select_waiter_worker_mask, bit, memory_order_release);
}

static inline void xr_channel_clear_waiter_bit(XrChannel *ch, int worker_id) {
    uint64_t bit = xr_channel_worker_bit(worker_id);
    if (!ch || bit == 0)
        return;

    uint64_t clear = ~bit;
    atomic_fetch_and_explicit(&ch->waiter_worker_mask, clear, memory_order_release);
    atomic_fetch_and_explicit(&ch->sender_waiter_worker_mask, clear, memory_order_release);
    atomic_fetch_and_explicit(&ch->receiver_waiter_worker_mask, clear, memory_order_release);
    atomic_fetch_and_explicit(&ch->select_waiter_worker_mask, clear, memory_order_release);
}

static inline void xr_channel_clear_all_waiter_masks(XrChannel *ch) {
    if (!ch)
        return;
    atomic_store_explicit(&ch->waiter_worker_mask, 0, memory_order_relaxed);
    atomic_store_explicit(&ch->sender_waiter_worker_mask, 0, memory_order_relaxed);
    atomic_store_explicit(&ch->receiver_waiter_worker_mask, 0, memory_order_relaxed);
    atomic_store_explicit(&ch->select_waiter_worker_mask, 0, memory_order_relaxed);
}

static inline void xr_channel_clear_sender_waiter_bit(XrChannel *ch, int worker_id) {
    uint64_t bit = xr_channel_worker_bit(worker_id);
    if (!ch || bit == 0)
        return;
    atomic_fetch_and_explicit(&ch->sender_waiter_worker_mask, ~bit, memory_order_release);
}

static inline void xr_channel_clear_receiver_waiter_bit(XrChannel *ch, int worker_id) {
    uint64_t bit = xr_channel_worker_bit(worker_id);
    if (!ch || bit == 0)
        return;
    atomic_fetch_and_explicit(&ch->receiver_waiter_worker_mask, ~bit, memory_order_release);
}

static inline void xr_channel_clear_select_waiter_bit(XrChannel *ch, int worker_id) {
    uint64_t bit = xr_channel_worker_bit(worker_id);
    if (!ch || bit == 0)
        return;
    atomic_fetch_and_explicit(&ch->select_waiter_worker_mask, ~bit, memory_order_release);
}

static inline uint64_t xr_channel_any_waiter_mask(XrChannel *ch) {
    if (!ch)
        return 0;
    return atomic_load_explicit(&ch->waiter_worker_mask, memory_order_acquire);
}

static inline uint64_t xr_channel_select_waiter_mask(XrChannel *ch) {
    if (!ch)
        return 0;
    return atomic_load_explicit(&ch->select_waiter_worker_mask, memory_order_acquire);
}

static inline uint64_t xr_channel_preferred_wake_mask(XrChannel *ch, bool wake_sender) {
    if (!ch)
        return 0;
    _Atomic(uint64_t) *dir_mask =
        wake_sender ? &ch->sender_waiter_worker_mask : &ch->receiver_waiter_worker_mask;
    uint64_t mask = atomic_load_explicit(dir_mask, memory_order_acquire);
    mask |= atomic_load_explicit(&ch->select_waiter_worker_mask, memory_order_acquire);
    return mask;
}

XR_FUNC XrChannelKind xr_channel_logical_kind_snapshot(XrChannel *ch);
XR_FUNC void xr_channel_record_ready_wake_metric(struct XrRuntime *runtime, XrChannelKind kind,
                                                 bool wake_sender);
XR_FUNC void xr_channel_record_ready_wake_retarget_metric(struct XrRuntime *runtime,
                                                          XrChannelKind kind, bool wake_sender);

/* ========== Channel API ========== */

XR_FUNC XrChannel *xr_channel_new(struct XrRuntimeCore *core, struct XrRuntime *scheduler,
                                  uint32_t buffer_size);
XR_FUNC XrChannel *xr_channel_new_vm(struct XrVMRuntime *X, uint32_t buffer_size);
XR_FUNC XrChannel *xr_channel_new_timer(struct XrRuntimeCore *core, struct XrRuntime *scheduler,
                                        int64_t timeout_ms);
XR_FUNC XrChannel *xr_channel_new_timer_vm(struct XrVMRuntime *X, int64_t timeout_ms);
XR_FUNC void xr_channel_timer_arm(XrChannel *ch, XrTimerWheel *tw);
// Release a select-owned timer channel (drops the select handle reference, and
// cancels the still-armed wheel timer when on its owner worker). See design/885.
XR_FUNC void xr_channel_timer_dispose(XrChannel *ch);
XR_FUNC bool xr_channel_timer_ready(XrChannel *ch);
XR_FUNC void xr_channel_destroy(XrChannel *ch);
XR_FUNC bool xr_channel_try_send(XrChannel *ch, XrValue value);

// Send value and wake any blocked receiver (for C code outside VM).
// Unlike try_send, this properly wakes receivers via resume_status protocol.
// Used by monitor exit notifications and other internal C-level senders.
XR_FUNC bool xr_channel_notify_send(XrChannel *ch, XrValue value);
XR_FUNC XrValue xr_channel_try_recv(XrChannel *ch, bool *ok);
XR_FUNC void xr_channel_close(XrChannel *ch);
XR_FUNC bool xr_channel_is_closed(XrChannel *ch);

/* ========== Blocking Operation Results ========== */

typedef enum {
    XR_CHAN_OK,
    XR_CHAN_CLOSED,
    XR_CHAN_BLOCK,
    XR_CHAN_NO_CORO,
    XR_CHAN_FULL  // pending request table saturated (backpressure)
} XrChanResult;

XR_FUNC XrChanResult xr_channel_send(XrChannel *ch, XrValue value, struct XrCoroutine *coro,
                                     int64_t timeout_ms);
XR_FUNC XrChanResult xr_channel_recv(XrChannel *ch, XrValue *out, struct XrCoroutine *coro,
                                     int64_t timeout_ms);
/* Blocking receive with waker-side delivery slots.
 * recv_slot: where the waker stores the received value.
 * ok_slot + deliver=true (untimed VM bytecode recv only) registers the
 * delivery capability: verified TRANSFER/shared payloads are stored with the
 * ok flag and the coroutine resumes at the NEXT instruction — no instruction
 * replay (kotlinx resume-with-value shape). deliver=false keeps the replay
 * contract for timeout variants, cfunc/AOT continuations and method calls. */
XR_FUNC XrChanResult xr_channel_recv_slot(XrChannel *ch, XrValue *out, struct XrCoroutine *coro,
                                          int64_t timeout_ms, XrSlotRef recv_slot,
                                          XrSlotRef ok_slot, bool deliver);

/* ========== Diagnostics ========== */

// Get channel close count for this runtime core (for leak detection).
// Reads from XrSystemHeap::stats.channel_close_count.
XR_FUNC uint64_t xr_channel_get_close_count(struct XrRuntimeCore *core);

/* ========== Channel Value Macros ========== */

static inline XrValue xr_value_from_channel(XrChannel *ch) {
    return XR_FROM_PTR(ch);
}

/* A value states its own type in the tag it carries, which every other
 * XR_IS_* predicate reads. Reading the pointed-to object header instead asks a
 * different question, and only the pointers that actually address a standard
 * header can answer it: for a value whose payload has another shape the read
 * lands on unrelated memory and reports whatever type tag that memory happens
 * to spell. */
static inline bool xr_value_is_channel(XrValue v) {
    return XR_IS_CHANNEL(v);
}

static inline XrChannel *xr_value_to_channel(XrValue v) {
    return (XrChannel *) XR_TO_PTR(v);
}

#endif  // XCHANNEL_H
