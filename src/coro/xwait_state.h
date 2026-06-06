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
typedef struct XrTask XrTask;
typedef struct XrArray XrArray;
typedef struct XrScopeContext XrScopeContext;
typedef struct XrBlockedBucket XrBlockedBucket;

/* ========== Await State Machine ========== */

typedef enum {
    XR_AWAIT_NONE = 0,      // not awaited (initial / consumed)
    XR_AWAIT_WAITING = 1,   // parent suspended, waiting for child
    XR_AWAIT_RESOLVED = 2,  // child completed, result in coro->result
} XrAwaitState;

typedef enum {
    XR_AWAIT_WAIT_IDLE = 0,
    XR_AWAIT_WAIT_REGISTERING,
    XR_AWAIT_WAIT_REGISTERED,
    XR_AWAIT_WAIT_RESOLVED,
    XR_AWAIT_WAIT_CANCELLED,
    XR_AWAIT_WAIT_TIMED_OUT,
} XrAwaitWaitTokenState;

typedef struct XrAwaitWaitToken {
    _Atomic int state;
    _Atomic(XrTask *) task;
    int waiter_index;
    uint32_t sequence;
} XrAwaitWaitToken;

static inline void xr_await_wait_token_reset(XrAwaitWaitToken *token) {
    if (!token)
        return;
    atomic_store_explicit(&token->state, XR_AWAIT_WAIT_IDLE, memory_order_relaxed);
    atomic_store_explicit(&token->task, NULL, memory_order_relaxed);
    token->waiter_index = -1;
}

static inline void xr_await_wait_token_prepare(XrAwaitWaitToken *token, XrTask *task,
                                               int waiter_index) {
    if (!token)
        return;
    token->sequence++;
    token->waiter_index = waiter_index;
    atomic_store_explicit(&token->task, task, memory_order_relaxed);
    atomic_store_explicit(&token->state, XR_AWAIT_WAIT_REGISTERING, memory_order_release);
}

static inline void xr_await_wait_token_commit(XrAwaitWaitToken *token) {
    if (!token)
        return;
    int expected = XR_AWAIT_WAIT_REGISTERING;
    (void) atomic_compare_exchange_strong_explicit(&token->state, &expected,
                                                   XR_AWAIT_WAIT_REGISTERED, memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline void xr_await_wait_token_set_terminal(XrAwaitWaitToken *token, int terminal) {
    if (!token)
        return;
    int state = atomic_load_explicit(&token->state, memory_order_acquire);
    while (state == XR_AWAIT_WAIT_REGISTERING || state == XR_AWAIT_WAIT_REGISTERED) {
        if (atomic_compare_exchange_weak_explicit(&token->state, &state, terminal,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static inline void xr_await_wait_token_resolve(XrAwaitWaitToken *token) {
    xr_await_wait_token_set_terminal(token, XR_AWAIT_WAIT_RESOLVED);
}

static inline void xr_await_wait_token_cancel(XrAwaitWaitToken *token) {
    xr_await_wait_token_set_terminal(token, XR_AWAIT_WAIT_CANCELLED);
}

static inline void xr_await_wait_token_timeout(XrAwaitWaitToken *token) {
    xr_await_wait_token_set_terminal(token, XR_AWAIT_WAIT_TIMED_OUT);
}

static inline void xr_await_wait_token_finish(XrAwaitWaitToken *token) {
    xr_await_wait_token_reset(token);
}

/* ========== Multi Await Wait Token ==========
 *
 * await all / await any register one coroutine against many tasks.  The
 * aggregate token gives cancellation and recycle paths an owner-side handle
 * for unregistering the task waiters before the coroutine shell can be reused.
 */
typedef enum {
    XR_MULTI_AWAIT_WAIT_IDLE = 0,
    XR_MULTI_AWAIT_WAIT_REGISTERING,
    XR_MULTI_AWAIT_WAIT_REGISTERED,
    XR_MULTI_AWAIT_WAIT_RESOLVED,
    XR_MULTI_AWAIT_WAIT_CANCELLED,
} XrMultiAwaitWaitTokenState;

typedef enum {
    XR_MULTI_AWAIT_NONE = 0,
    XR_MULTI_AWAIT_ALL,
    XR_MULTI_AWAIT_ANY,
    XR_MULTI_AWAIT_ANY_SUCCESS,
} XrMultiAwaitMode;

typedef struct XrMultiAwaitWaitToken {
    _Atomic int state;
    _Atomic(XrArray *) tasks;
    int mode;
    int task_count;
    uint32_t sequence;
} XrMultiAwaitWaitToken;

static inline void xr_multi_await_wait_token_reset(XrMultiAwaitWaitToken *token) {
    if (!token)
        return;
    atomic_store_explicit(&token->state, XR_MULTI_AWAIT_WAIT_IDLE, memory_order_relaxed);
    atomic_store_explicit(&token->tasks, NULL, memory_order_relaxed);
    token->mode = XR_MULTI_AWAIT_NONE;
    token->task_count = 0;
}

static inline void xr_multi_await_wait_token_prepare(XrMultiAwaitWaitToken *token, XrArray *tasks,
                                                     int mode, int task_count) {
    if (!token)
        return;
    token->sequence++;
    atomic_store_explicit(&token->tasks, tasks, memory_order_relaxed);
    token->mode = mode;
    token->task_count = task_count;
    atomic_store_explicit(&token->state, XR_MULTI_AWAIT_WAIT_REGISTERING, memory_order_release);
}

static inline void xr_multi_await_wait_token_commit(XrMultiAwaitWaitToken *token) {
    if (!token)
        return;
    int expected = XR_MULTI_AWAIT_WAIT_REGISTERING;
    (void) atomic_compare_exchange_strong_explicit(&token->state, &expected,
                                                   XR_MULTI_AWAIT_WAIT_REGISTERED,
                                                   memory_order_acq_rel, memory_order_acquire);
}

static inline void xr_multi_await_wait_token_set_terminal(XrMultiAwaitWaitToken *token,
                                                          int terminal) {
    if (!token)
        return;
    int state = atomic_load_explicit(&token->state, memory_order_acquire);
    while (state == XR_MULTI_AWAIT_WAIT_REGISTERING || state == XR_MULTI_AWAIT_WAIT_REGISTERED) {
        if (atomic_compare_exchange_weak_explicit(&token->state, &state, terminal,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static inline void xr_multi_await_wait_token_resolve(XrMultiAwaitWaitToken *token) {
    xr_multi_await_wait_token_set_terminal(token, XR_MULTI_AWAIT_WAIT_RESOLVED);
}

static inline void xr_multi_await_wait_token_cancel(XrMultiAwaitWaitToken *token) {
    xr_multi_await_wait_token_set_terminal(token, XR_MULTI_AWAIT_WAIT_CANCELLED);
}

static inline void xr_multi_await_wait_token_finish(XrMultiAwaitWaitToken *token) {
    xr_multi_await_wait_token_reset(token);
}

/* ========== Scope Wait Token ==========
 *
 * Scope exit blocks the owner until all children complete.  The child side
 * decrements XrScopeContext::count; this token records the owner-side wait
 * lifecycle so scope wait follows the same explicit commit/terminal contract
 * as await, channel and select waiters.
 */
typedef enum {
    XR_SCOPE_WAIT_IDLE = 0,
    XR_SCOPE_WAIT_REGISTERING,
    XR_SCOPE_WAIT_REGISTERED,
    XR_SCOPE_WAIT_RESOLVED,
    XR_SCOPE_WAIT_CANCELLED,
} XrScopeWaitTokenState;

typedef struct XrScopeWaitToken {
    _Atomic int state;
    _Atomic(XrScopeContext *) scope;
    uint32_t sequence;
} XrScopeWaitToken;

static inline void xr_scope_wait_token_reset(XrScopeWaitToken *token) {
    if (!token)
        return;
    atomic_store_explicit(&token->state, XR_SCOPE_WAIT_IDLE, memory_order_relaxed);
    atomic_store_explicit(&token->scope, NULL, memory_order_relaxed);
}

static inline void xr_scope_wait_token_prepare(XrScopeWaitToken *token, XrScopeContext *scope) {
    if (!token)
        return;
    token->sequence++;
    atomic_store_explicit(&token->scope, scope, memory_order_relaxed);
    atomic_store_explicit(&token->state, XR_SCOPE_WAIT_REGISTERING, memory_order_release);
}

static inline void xr_scope_wait_token_commit(XrScopeWaitToken *token) {
    if (!token)
        return;
    int expected = XR_SCOPE_WAIT_REGISTERING;
    (void) atomic_compare_exchange_strong_explicit(&token->state, &expected,
                                                   XR_SCOPE_WAIT_REGISTERED, memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline void xr_scope_wait_token_set_terminal(XrScopeWaitToken *token, int terminal) {
    if (!token)
        return;
    int state = atomic_load_explicit(&token->state, memory_order_acquire);
    while (state == XR_SCOPE_WAIT_REGISTERING || state == XR_SCOPE_WAIT_REGISTERED) {
        if (atomic_compare_exchange_weak_explicit(&token->state, &state, terminal,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static inline void xr_scope_wait_token_resolve(XrScopeWaitToken *token) {
    xr_scope_wait_token_set_terminal(token, XR_SCOPE_WAIT_RESOLVED);
}

static inline void xr_scope_wait_token_cancel(XrScopeWaitToken *token) {
    xr_scope_wait_token_set_terminal(token, XR_SCOPE_WAIT_CANCELLED);
}

static inline void xr_scope_wait_token_finish(XrScopeWaitToken *token) {
    xr_scope_wait_token_reset(token);
}

/* ========== Timer Wait Token ==========
 *
 * XrTWheelTimer is the wheel node.  This token is the coroutine-side view:
 * which worker owns the timer, which wait reason armed it, and whether the
 * timer fired or was cancelled by another wake path before it fired.
 */
typedef enum {
    XR_TIMER_WAIT_IDLE = 0,
    XR_TIMER_WAIT_REGISTERING,
    XR_TIMER_WAIT_REGISTERED,
    XR_TIMER_WAIT_FIRED,
    XR_TIMER_WAIT_CANCELLED,
} XrTimerWaitTokenState;

typedef struct XrTimerWaitToken {
    _Atomic int state;
    int owner_worker_id;
    int wait_reason;
    int64_t deadline_ticks;
    uint32_t sequence;
} XrTimerWaitToken;

static inline void xr_timer_wait_token_reset(XrTimerWaitToken *token) {
    if (!token)
        return;
    atomic_store_explicit(&token->state, XR_TIMER_WAIT_IDLE, memory_order_relaxed);
    token->owner_worker_id = -1;
    token->wait_reason = 0;
    token->deadline_ticks = 0;
}

static inline void xr_timer_wait_token_prepare(XrTimerWaitToken *token, int owner_worker_id,
                                               int wait_reason, int64_t deadline_ticks) {
    if (!token)
        return;
    token->sequence++;
    token->owner_worker_id = owner_worker_id;
    token->wait_reason = wait_reason;
    token->deadline_ticks = deadline_ticks;
    atomic_store_explicit(&token->state, XR_TIMER_WAIT_REGISTERING, memory_order_release);
}

static inline void xr_timer_wait_token_commit(XrTimerWaitToken *token) {
    if (!token)
        return;
    int expected = XR_TIMER_WAIT_REGISTERING;
    (void) atomic_compare_exchange_strong_explicit(&token->state, &expected,
                                                   XR_TIMER_WAIT_REGISTERED, memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline void xr_timer_wait_token_set_terminal(XrTimerWaitToken *token, int terminal) {
    if (!token)
        return;
    int state = atomic_load_explicit(&token->state, memory_order_acquire);
    while (state == XR_TIMER_WAIT_REGISTERING || state == XR_TIMER_WAIT_REGISTERED) {
        if (atomic_compare_exchange_weak_explicit(&token->state, &state, terminal,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static inline void xr_timer_wait_token_fire(XrTimerWaitToken *token) {
    xr_timer_wait_token_set_terminal(token, XR_TIMER_WAIT_FIRED);
}

static inline void xr_timer_wait_token_cancel(XrTimerWaitToken *token) {
    xr_timer_wait_token_set_terminal(token, XR_TIMER_WAIT_CANCELLED);
}

static inline void xr_timer_wait_token_finish(XrTimerWaitToken *token) {
    xr_timer_wait_token_reset(token);
}

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

typedef enum {
    XR_SELECT_WAIT_IDLE = 0,
    XR_SELECT_WAIT_REGISTERING,
    XR_SELECT_WAIT_REGISTERED,
    XR_SELECT_WAIT_RESOLVED,
    XR_SELECT_WAIT_CANCELLED,
    XR_SELECT_WAIT_TIMED_OUT,
} XrSelectWaitTokenState;

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
    _Atomic int state;
    _Atomic bool active;
    _Atomic bool triggered;
    _Atomic int selected_index;
    _Atomic int selected_status;
    uint32_t sequence;
} XrSelectWait;

static inline void xr_select_wait_prepare(XrSelectWait *wait) {
    if (!wait)
        return;
    wait->sequence++;
    atomic_store_explicit(&wait->state, XR_SELECT_WAIT_REGISTERING, memory_order_release);
}

static inline void xr_select_wait_commit(XrSelectWait *wait) {
    if (!wait)
        return;
    int expected = XR_SELECT_WAIT_REGISTERING;
    (void) atomic_compare_exchange_strong_explicit(&wait->state, &expected,
                                                   XR_SELECT_WAIT_REGISTERED, memory_order_acq_rel,
                                                   memory_order_acquire);
}

static inline void xr_select_wait_set_terminal(XrSelectWait *wait, int terminal) {
    if (!wait)
        return;
    int state = atomic_load_explicit(&wait->state, memory_order_acquire);
    while (state == XR_SELECT_WAIT_REGISTERING || state == XR_SELECT_WAIT_REGISTERED) {
        if (atomic_compare_exchange_weak_explicit(&wait->state, &state, terminal,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            return;
        }
    }
}

static inline void xr_select_wait_resolve(XrSelectWait *wait) {
    xr_select_wait_set_terminal(wait, XR_SELECT_WAIT_RESOLVED);
}

static inline void xr_select_wait_cancel(XrSelectWait *wait) {
    xr_select_wait_set_terminal(wait, XR_SELECT_WAIT_CANCELLED);
}

static inline void xr_select_wait_timeout(XrSelectWait *wait) {
    xr_select_wait_set_terminal(wait, XR_SELECT_WAIT_TIMED_OUT);
}

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
    XrAwaitWaitToken await_token;
    XrMultiAwaitWaitToken multi_await_token;
    XrScopeWaitToken scope_token;
    XrTimerWaitToken timer_token;
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
