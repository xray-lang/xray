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
 *   wait_link pointers. A coroutine is on at most one wait queue
 *   at a time. Enqueue/dequeue are always performed under the lock.
 *
 *   INVARIANT 4 (Close semantics): Once closed is set to true, no
 *   further sends are allowed (return XR_CHAN_CLOSED). Pending
 *   receivers are woken with null. Pending senders are woken with
 *   XR_CHAN_CLOSED. Buffered items remain readable after close
 *   until the buffer is drained.
 *
 *   INVARIANT 5 (Lock discipline): All mutations to buffer state
 *   (buf_count, send_idx, recv_idx) and wait queues (sendq, recvq)
 *   must be performed under the channel mutex (XrMutex). The lock
 *   is held for short durations only (no blocking operations under
 *   lock). XrMutex is an adaptive 3-state lock (active spin -> yield
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

/* ========== Forward Declarations ========== */

typedef struct XrCoroutine XrCoroutine;
typedef struct XrScheduler XrScheduler;

/* ========== Channel Lock ========== */

#include "../base/xmutex.h"

/* ========== Wait Queue ========== */

typedef struct XrWaitQueue {
    XrCoroutine *first;
    XrCoroutine *last;
} XrWaitQueue;

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
XR_FUNC void xr_channel_remove_waiter(struct XrChannel *ch, XrCoroutine *coro);

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
    int  (*send)(struct XrChannel *ch, XrValue value, struct XrCoroutine *coro);
    int  (*recv)(struct XrChannel *ch, XrValue *out, struct XrCoroutine *coro);
    bool (*try_send)(struct XrChannel *ch, XrValue value);
    XrValue (*try_recv)(struct XrChannel *ch, bool *ok);
    void (*close)(struct XrChannel *ch);
    void (*destroy)(struct XrChannel *ch);
    void (*on_select_enter)(struct XrChannel *ch);
    void (*on_select_exit)(struct XrChannel *ch);
} XrChannelDistHooks;

// Global hook pointer, set by cluster module at startup. NULL = local only.
XR_DATA XrChannelDistHooks *xr_channel_dist_hooks;

/* ========== Channel Structure ========== */

typedef struct XrChannel {
    XrGCHeader gc_header;

    /* === Buffer (ring buffer for buffered channels) === */
    XrValue *buffer;          // NULL for unbuffered
    uint32_t buf_size;        // 0 for unbuffered
    uint32_t buf_count;       // Current item count
    uint32_t send_idx;        // Next write position
    uint32_t recv_idx;        // Next read position

    /* === Wait Queues === */
    XrWaitQueue sendq;        // Blocked senders
    XrWaitQueue recvq;        // Blocked receivers

    /* === State (atomic) === */
    _Atomic(bool) closed;
    XrMutex lock;

    /* === Timer Channel === */
    _Atomic(bool) is_timer;
    int64_t timer_timeout_ms;
    int64_t timer_start_ticks;
    _Atomic(bool) timer_fired;
    uint8_t elem_tid;           // XrTypeId: element type for reified generics (0=any)

    /* === Distributed Channel (cluster) === */
    void *dist;               // Opaque pointer to cluster dist context (NULL = local)
    const char *name;         // Named Channel identifier (NULL = anonymous)
} XrChannel;

/* ========== Channel API ========== */

XR_FUNC XrChannel *xr_channel_new(struct XrayIsolate *X, uint32_t buffer_size);
XR_FUNC XrChannel *xr_channel_new_timer(struct XrayIsolate *X, int64_t timeout_ms);
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
    XR_CHAN_NO_CORO
} XrChanResult;

XR_FUNC XrChanResult xr_channel_send(XrChannel *ch, XrValue value, struct XrCoroutine *coro);
XR_FUNC XrChanResult xr_channel_recv(XrChannel *ch, XrValue *out, struct XrCoroutine *coro);

/* ========== Diagnostics ========== */

// Get global channel close count (for leak detection)
XR_FUNC uint64_t xr_channel_get_close_count(void);

/* ========== Channel Value Macros ========== */

static inline XrValue xr_value_from_channel(XrChannel *ch) {
    return XR_FROM_PTR(ch);
}

static inline bool xr_value_is_channel(XrValue v) {
    if (!XR_IS_PTR(v)) return false;
    return XR_GC_GET_TYPE((XrGCHeader*)XR_TO_PTR(v)) == XR_TCHANNEL;
}

static inline XrChannel *xr_value_to_channel(XrValue v) {
    return (XrChannel*)XR_TO_PTR(v);
}

#endif // XCHANNEL_H
