/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtask.h - GC-managed coroutine handle with structured concurrency support
 *
 * KEY CONCEPT:
 *   XrTask is the user-visible handle returned by `go` expressions.
 *   It supports parent-child hierarchy for structured concurrency,
 *   completion listeners for async notification, and a 6-state
 *   state machine for precise lifecycle tracking.
 *
 * WHY THIS DESIGN:
 *   - Decouples user handle (GC-managed, ~96B) from executor (pool-managed, ~500B)
 *   - Parent-child hierarchy enables linked/monitored go and scope blocks
 *   - CompletionNode allows multiple listeners (monitor channels, callbacks)
 *   - 6-state machine tracks Completing/Cancelling transitions for children
 *
 * RELATED MODULES:
 *   - xcoroutine.h: Executor (pool-allocated execution context)
 *   - xvm_cold_paths.c: vm_await reads task->state/result
 *   - xworker.c: executor_complete writes task->result, recycles executor
 *   - linked go / monitored go syntax
 */

#ifndef XTASK_H
#define XTASK_H

#include <stdatomic.h>
#include <stdbool.h>
#include "../runtime/gc/xgc_header.h"
#include "../runtime/value/xvalue.h"

/* ========== Forward Declarations ========== */

struct XrCoroutine;
struct XrArray;
struct XrayIsolate;
struct XrChannel;

/* ========== Task State (6-state machine) ========== */
/*
 *                                   wait children
 *  +--------+ self done  +-------------+  all done  +-----------+
 *  | Active | ---------> | Completing  | ---------> | Completed |
 *  +--------+            +-------------+            +-----------+
 *     |  cancel               |
 *     V                       V
 *  +------------+  children done  +-----------+
 *  | Cancelling | --------------> | Cancelled |
 *  +------------+                 +-----------+
 *
 *  +--------+  error  +-----------+
 *  | Active | ------> |  Failed   |  (no children, or after cancel children)
 *  +--------+         +-----------+
 */
typedef enum {
    XR_TASK_ACTIVE      = 0,  // executor running
    XR_TASK_COMPLETING  = 1,  // self done, waiting for children
    XR_TASK_CANCELLING  = 2,  // cancel requested, children still running
    XR_TASK_COMPLETED   = 3,  // final: success
    XR_TASK_FAILED      = 4,  // final: error
    XR_TASK_CANCELLED   = 5,  // final: cancelled
} XrTaskState;

/* ========== Link Mode (go prefix modifier) ========== */

typedef enum {
    XR_LINK_NONE      = 0,  // go fn()          — independent (default)
    XR_LINK_LINKED    = 1,  // linked go fn()   — bidirectional error propagation
    XR_LINK_MONITORED = 2,  // monitored go fn() — one-way completion notification
} XrLinkMode;

/* ========== Scope Mode (scope prefix modifier) ========== */

typedef enum {
    XR_SCOPE_WAIT       = 0,  // scope { } — wait barrier (default)
    XR_SCOPE_LINKED     = 1,  // linked scope { } — child fail cancels all + throws
    XR_SCOPE_SUPERVISOR = 2,  // supervisor scope { } — collect errors, no cancel
} XrScopeMode;

/* ========== Task Flags ========== */

#define XR_TASK_FLG_SUPERVISOR   (1 << 0)  // child error doesn't propagate up
#define XR_TASK_FLG_SCOPE_TASK   (1 << 1)  // implicit task created by scope block
#define XR_TASK_FLG_HAS_PARENT   (1 << 2)  // attached to a parent task

/* ========== Completion Listener ========== */

typedef enum {
    XR_COMPLETION_WAKE      = 0,  // resume a blocked coroutine
    XR_COMPLETION_CHANNEL   = 1,  // send event to Channel (for monitor)
    XR_COMPLETION_CLOSURE   = 2,  // call user closure (onComplete)
} XrCompletionType;

typedef struct XrCompletionNode {
    struct XrCompletionNode *next;
    uint8_t type;                   // XrCompletionType
    uint8_t _pad[7];
    union {
        struct XrCoroutine *waiter;        // WAKE
        struct XrChannel *channel;         // CHANNEL
        XrValue closure;                   // CLOSURE (16B)
    } as;
} XrCompletionNode;

/* ========== Task Link (bidirectional association) ========== */

typedef struct XrTaskLink {
    struct XrTask *peer;          // linked peer task
    struct XrTaskLink *next;      // next link in this task's list
} XrTaskLink;

/* ========== XrTask - GC-managed coroutine handle ========== */

typedef struct XrTask {
    // GC header (must be first field)
    XrGCHeader gc;                          // 16B

    // Cached result — survives executor recycling
    XrValue result;                         // 16B
    XrValue error;                          // 16B

    // Back-pointer to executor (NULL after completion + recycle)
    struct XrCoroutine *coro;               //  8B

    // State machine + flags
    _Atomic uint8_t state;                  //  1B
    uint8_t flags;                          //  1B
    uint8_t link_mode;                      //  1B: XR_LINK_NONE/LINKED/MONITORED
    uint8_t _pad1;                          //  1B
    uint16_t child_count;                   //  2B
    uint16_t _pad2;                         //  2B

    // Parent-Child hierarchy (only used with linked go / scope)
    struct XrTask *parent;                  //  8B
    struct XrTask *first_child;             //  8B
    struct XrTask *next_sibling;            //  8B

    // Bidirectional link peers (task.link() API)
    struct XrTaskLink *links;               //  8B

    // Completion listeners
    struct XrCompletionNode *on_completion; //  8B

    // Await coordination (single CAS protocol, mirrors old coro->await_state)
    _Atomic int await_state;                //  4B: NONE / WAITING / RESOLVED
    int waiter_index;                       //  4B: -1=single, -2=scope, -3=any, >=0=all[idx]
    struct XrCoroutine *waiter;             //  8B: coro waiting on this task
} XrTask;
// ~112B total

/* ========== Task Lifecycle API ========== */

/* Allocate a new task on executor's GC heap.
 * Links task->coro = executor, executor->task = task. */
XR_FUNC struct XrTask *xr_task_create(struct XrCoroutine *parent_coro,
                                       struct XrCoroutine *executor);

// Simple state setters (called from xworker.c on executor completion)
XR_FUNC void xr_task_complete(struct XrTask *task, XrValue result);
XR_FUNC void xr_task_fail(struct XrTask *task, XrValue error);
XR_FUNC void xr_task_cancel(struct XrTask *task);

/* ========== Structured Concurrency API ========== */

// Attach child to parent's child list
XR_FUNC void xr_task_attach_child(struct XrTask *parent, struct XrTask *child);

// Detach child from parent's child list
XR_FUNC void xr_task_detach_child(struct XrTask *parent, struct XrTask *child);

// Complete with children support: has children → COMPLETING, else → finalize
XR_FUNC void xr_task_try_complete(struct XrTask *task, XrValue result);

// Transition to terminal state + notify parent + fire listeners
XR_FUNC void xr_task_finalize(struct XrTask *task, uint8_t final_state);

// Called when a child reaches terminal state
XR_FUNC void xr_task_child_completed(struct XrTask *parent, struct XrTask *child);

// Cancel a task and all its children recursively
XR_FUNC void xr_task_cancel_tree(struct XrTask *task);

// Fail with upward propagation (skips supervisor parents)
XR_FUNC void xr_task_fail_with_propagation(struct XrTask *task, XrValue error);

// Fire all completion listeners
XR_FUNC void xr_task_fire_completion(struct XrTask *task);

// Bidirectional link: a fails → cancel b, b fails → cancel a
XR_FUNC void xr_task_link(struct XrTask *a, struct XrTask *b);

// Remove bidirectional link between a and b
XR_FUNC void xr_task_unlink(struct XrTask *a, struct XrTask *b);

// Add a completion listener
XR_FUNC void xr_task_add_completion(struct XrTask *task, struct XrCompletionNode *node);

// Wake the waiter registered on this task (replaces xr_coro_wake_waiter for Task path)
XR_FUNC void xr_task_wake_waiter(struct XrayIsolate *X, struct XrTask *task);

/* ========== Task State Helpers (inline) ========== */

static inline bool xr_task_is_active(const struct XrTask *task) {
    uint8_t s = atomic_load_explicit(&((struct XrTask *)task)->state, memory_order_acquire);
    return s <= XR_TASK_COMPLETING;
}

static inline bool xr_task_is_done(const struct XrTask *task) {
    uint8_t s = atomic_load_explicit(&((struct XrTask *)task)->state, memory_order_acquire);
    return s >= XR_TASK_COMPLETED;
}

static inline bool xr_task_is_cancelled(const struct XrTask *task) {
    uint8_t s = atomic_load_explicit(&((struct XrTask *)task)->state, memory_order_acquire);
    return s == XR_TASK_CANCELLING || s == XR_TASK_CANCELLED;
}

#endif // XTASK_H
