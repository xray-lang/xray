/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwait_state.h - Coroutine wait/select/scope state layouts
 *
 * KEY CONCEPT:
 *   Blocking state is owned by the coroutine runtime, not by any execution
 *   backend. VM, JIT, AOT, and yieldable C functions all suspend through the
 *   same wait records and resume with backend-specific continuation state.
 */

#ifndef XWAIT_STATE_H
#define XWAIT_STATE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "../runtime/value/xvalue.h"
#include "xslot_ref.h"

typedef struct XrCoroutine XrCoroutine;
typedef struct XrBlockedBucket XrBlockedBucket;

/* ========== Await State Machine ========== */

typedef enum {
    XR_AWAIT_NONE = 0,      // not awaited (initial / consumed)
    XR_AWAIT_WAITING = 1,   // parent suspended, waiting for child
    XR_AWAIT_RESOLVED = 2,  // child completed, result in coro->result
} XrAwaitState;

/* ========== Channel Wait Token ==========
 *
 * A channel waiter has a short but race-sensitive lifecycle:
 *
 *   IDLE -> REGISTERING -> REGISTERED -> RESOLVED
 *                                  \----> TIMED_OUT
 *                                  \----> CANCELLED
 *
 * The channel lock serializes REGISTERING/REGISTERED with wait queue
 * insertion.  Wake paths claim the coroutine with xr_coro_claim_wake(); this
 * token records the waiter-side decision so specialized channel queues share
 * the same commit/cancel contract without depending on ad hoc fields.
 */
typedef enum {
    XR_CHAN_WAIT_IDLE = 0,
    XR_CHAN_WAIT_REGISTERING,
    XR_CHAN_WAIT_REGISTERED,
    XR_CHAN_WAIT_RESOLVED,
    XR_CHAN_WAIT_CANCELLED,
    XR_CHAN_WAIT_TIMED_OUT,
} XrChannelWaitTokenState;

typedef struct XrChannelWaitToken {
    _Atomic int state;
    _Atomic(void *) channel;
    bool is_send;
    uint32_t sequence;
} XrChannelWaitToken;

static inline void xr_channel_wait_token_reset(XrChannelWaitToken *token) {
    if (!token)
        return;
    atomic_store_explicit(&token->state, XR_CHAN_WAIT_IDLE, memory_order_relaxed);
    atomic_store_explicit(&token->channel, NULL, memory_order_relaxed);
    token->is_send = false;
}

static inline void xr_channel_wait_token_prepare(XrChannelWaitToken *token, void *channel,
                                                 bool is_send) {
    if (!token)
        return;
    token->sequence++;
    token->is_send = is_send;
    atomic_store_explicit(&token->channel, channel, memory_order_relaxed);
    atomic_store_explicit(&token->state, XR_CHAN_WAIT_REGISTERING, memory_order_release);
}

static inline void xr_channel_wait_token_commit(XrChannelWaitToken *token) {
    if (!token)
        return;
    int expected = XR_CHAN_WAIT_REGISTERING;
    if (atomic_compare_exchange_strong_explicit(&token->state, &expected, XR_CHAN_WAIT_REGISTERED,
                                                memory_order_acq_rel, memory_order_acquire)) {
        return;
    }
}

static inline void xr_channel_wait_token_finish(XrChannelWaitToken *token) {
    xr_channel_wait_token_reset(token);
}

static inline void xr_channel_wait_token_set_terminal(XrChannelWaitToken *token, int terminal) {
    if (!token)
        return;
    int state = atomic_load_explicit(&token->state, memory_order_acquire);
    while (state == XR_CHAN_WAIT_REGISTERING || state == XR_CHAN_WAIT_REGISTERED) {
        if (atomic_compare_exchange_weak_explicit(&token->state, &state, terminal,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static inline void xr_channel_wait_token_resolve(XrChannelWaitToken *token) {
    xr_channel_wait_token_set_terminal(token, XR_CHAN_WAIT_RESOLVED);
}

static inline void xr_channel_wait_token_cancel(XrChannelWaitToken *token) {
    xr_channel_wait_token_set_terminal(token, XR_CHAN_WAIT_CANCELLED);
}

static inline void xr_channel_wait_token_timeout(XrChannelWaitToken *token) {
    xr_channel_wait_token_set_terminal(token, XR_CHAN_WAIT_TIMED_OUT);
}

/* ========== Select Support ========== */

#define XR_SELECT_INLINE_CASES 4

typedef struct XrSelectCase {
    void *channel;
    bool is_send;
    XrValue send_value;
    XrSlotRef result_slot;
    XrCoroutine *owner;
    struct XrSelectCase *prev;
    struct XrSelectCase *next;
    XrBlockedBucket *bucket;
} XrSelectCase;

typedef struct XrSelectWait {
    XrSelectCase *cases;
    int case_count;
    void *timer_channel;
    _Atomic bool active;
    _Atomic bool triggered;
    _Atomic int selected_index;
    _Atomic int selected_status;
} XrSelectWait;

typedef struct XrSelectStorage {
    XrSelectWait wait;
    XrSelectCase inline_cases[XR_SELECT_INLINE_CASES];
    XrSelectCase *heap_cases;
    int heap_capacity;
} XrSelectStorage;

typedef struct XrCoroWaitState {
    struct XrTask *_Atomic await_task;
    _Atomic int wait_count;
    _Atomic bool any_done;
} XrCoroWaitState;

struct XrBlockedBucket {
    void *channel;
    XrCoroutine *send_head;
    XrCoroutine *send_tail;
    XrCoroutine *recv_head;
    XrCoroutine *recv_tail;
    XrSelectCase *select_head;
    XrSelectCase *select_tail;
    struct XrBlockedBucket *next;
};

/* ========== Scope Context ==========
 *
 * XrScopeContext represents a single `scope { ... }` block at runtime
 * and is orthogonal to the XrTask tree (xtask.h). They both express
 * parent/child relationships, but answer different questions and must stay
 * separate:
 *
 *   XrTask tree (xtask.h)
 *     - one node per `go` expression
 *     - parent/child links describe who awaits whom
 *     - GC-managed, survives executor recycle
 *     - lives across the whole task lifecycle
 *
 *   XrScopeContext
 *     - one node per `scope { ... }`, `linked scope { ... }`, or
 *       `supervisor scope { ... }` block
 *     - first_child links describe which coroutines run inside this block
 *     - malloc-allocated, freed at OP_SCOPE_EXIT
 *     - carries the per-block policy that does not exist on XrTask
 *
 * A single coroutine has both a parent task and a parent scope. Each scope
 * block can contain multiple tasks; a top-level `go fn()` outside any scope
 * has no parent scope at all.
 *
 * Concurrency: child_lock serializes mutations of first_child, first_error,
 * errors[], and cancel_requested under the wake dispatcher. errors[] is
 * preallocated at OP_SCOPE_ENTER for the supervisor mode so the locked
 * section never needs to allocate. */

typedef struct XrScopeContext {
    _Atomic int count;
    struct XrScopeContext *parent;
    XrCoroutine *owner;
    uint8_t mode;                   // XrScopeMode
    _Atomic bool cancel_requested;  // linked scope: set when first child fails
    _Atomic bool child_lock;        // Spinlock; see lock contract above
    XrValue first_error;            // linked scope: first child error (lock-protected)
    bool first_error_is_value;      // linked scope: first_error came via value channel
    struct XrArray *errors;         // supervisor scope: collected errors
    XrCoroutine *first_child;       // linked list of child coroutines in this scope
} XrScopeContext;

#endif  // XWAIT_STATE_H
