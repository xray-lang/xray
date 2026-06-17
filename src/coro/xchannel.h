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
 *   - Distributed channel hooks for cluster Named Channel
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
 *   INVARIANT 6 (Distributed hooks): When dist != NULL, all send/
 *   recv/close operations are routed through xr_channel_dist_hooks.
 *   The hook layer is responsible for network serialization and
 *   remote delivery. Local buffer and wait queues are not used.
 */

#ifndef XCHANNEL_H
#define XCHANNEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "../runtime/value/xvalue.h"
#include "../runtime/gc/xgc_header.h"
#include "../base/xmalloc.h"
#include "../base/xchecks.h"
#include "xslot_ref.h"
#include "xtimer_wheel.h"  // XrTWheelTimer (embedded in channel for timer wheel)

/* ========== Forward Declarations ========== */

typedef struct XrCoroutine XrCoroutine;
typedef struct XrCoroState XrCoroState;

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

struct XrChannel;
// Remove coro from the channel's wait queue under ch->lock.
// Returns true when THIS call unlinked the coroutine — the caller now owns
// the wake decision (Go sudog semantics: dequeue under lock = exclusive
// ownership). Returns false when the coroutine was no longer queued, i.e.
// a concurrent send/recv/close already dequeued and owns it.
XR_FUNC bool xr_channel_remove_waiter(struct XrChannel *ch, XrCoroutine *coro);
XR_FUNC void xr_channel_lock_observed(struct XrChannel *ch);

/* ========== Distributed Channel Hooks ========== */

// Forward declare for hook signatures
struct XrChannel;
enum XrChanResult;

/*
 * Hook table for distributed (Named) channel operations.
 * When a channel has dist != NULL, send/recv/close are
 * routed through these hooks to the cluster layer.
 * Local channels (dist == NULL) are not affected.
 */
typedef struct XrChannelDistHooks {
    int (*send)(struct XrChannel *ch, XrValue value, struct XrCoroutine *coro);
    int (*recv)(struct XrChannel *ch, XrValue *out, struct XrCoroutine *coro);
    bool (*try_send)(struct XrChannel *ch, XrValue value);
    XrValue (*try_recv)(struct XrChannel *ch, bool *ok);
    void (*close)(struct XrChannel *ch);
    void (*destroy)(struct XrChannel *ch);
    void (*on_select_enter)(struct XrChannel *ch);
    void (*on_select_exit)(struct XrChannel *ch);
} XrChannelDistHooks;

// Hooks live on XrayIsolate (see XrayIsolate::channel_dist_hooks in
// xisolate_internal.h). Install/uninstall via xr_cluster_channel_install_hooks.

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
    XrGCHeader gc_header;

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

    /* === Distributed Channel (cluster) === */
    void *dist;        // Opaque pointer to cluster dist context (NULL = local)
    const char *name;  // Named Channel identifier (NULL = anonymous)

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

    /* === Owner Isolate (for dist hook dispatch + stats) === */
    struct XrayIsolate *isolate;  // Set at xr_channel_new
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

XR_FUNC XrChannel *xr_channel_new(struct XrayIsolate *X, uint32_t buffer_size);
XR_FUNC XrChannel *xr_channel_new_timer(struct XrayIsolate *X, int64_t timeout_ms);
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
 * delivery capability: when the woken value needs no receive-side deep
 * copy, the waker also stores the ok flag and the coroutine resumes at
 * the NEXT instruction — no instruction replay (kotlinx resume-with-value
 * shape). Values that need a receive-side deep copy fall back to the
 * replay/resume protocol per wake. deliver=false keeps that protocol
 * contract for timeout variants, cfunc/AOT continuations and
 * method-call paths. */
XR_FUNC XrChanResult xr_channel_recv_slot(XrChannel *ch, XrValue *out, struct XrCoroutine *coro,
                                          int64_t timeout_ms, XrSlotRef recv_slot,
                                          XrSlotRef ok_slot, bool deliver);

/* ========== Diagnostics ========== */

// Get channel close count for this isolate (for leak detection).
// Reads from XrSystemHeap::stats.channel_close_count.
XR_FUNC uint64_t xr_channel_get_close_count(struct XrayIsolate *X);

/* ========== Channel Value Macros ========== */

static inline XrValue xr_value_from_channel(XrChannel *ch) {
    return XR_FROM_PTR(ch);
}

static inline bool xr_value_is_channel(XrValue v) {
    if (!XR_IS_PTR(v))
        return false;
    return XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(v)) == XR_TCHANNEL;
}

static inline XrChannel *xr_value_to_channel(XrValue v) {
    return (XrChannel *) XR_TO_PTR(v);
}

#endif  // XCHANNEL_H
