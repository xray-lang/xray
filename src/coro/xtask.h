/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtask.h - Runtime-managed coroutine handle with structured concurrency support
 *
 * KEY CONCEPT:
 *   XrTask is the user-visible handle returned by `go` expressions.
 *   It supports parent-child hierarchy for structured concurrency,
 *   completion listeners for async notification, and a 6-state
 *   state machine for precise lifecycle tracking.
 *
 * WHY THIS DESIGN:
 *   - Decouples user handle (runtime-managed, ~128B) from executor
 *     (pool-managed VM/AOT execution context)
 *   - Parent-child hierarchy enables linked go and scope blocks
 *   - CompletionNode allows multiple listeners (monitor channels, callbacks)
 *   - 6-state machine tracks Completing/Cancelling transitions for children
 *
 * ORTHOGONAL TO XrScopeContext:
 *   The task tree (parent / first_child / next_sibling here) tracks
 *   "who awaits whom" — one node per `go` expression. The scope chain
 *   (XrScopeContext, xcoroutine.h) tracks "which coroutines run inside
 *   the same `scope { ... }` block" — one node per scope statement,
 *   carrying the structured-concurrency policy (plain wait / linked).
 *   They overlap in shape but capture different parent/child concepts;
 *   merging them would lose the policy dimension scope provides and
 *   would force every standalone `go` to materialize a fake scope.
 *   See the doc comment on XrScopeContext for the full breakdown.
 *
 * RELATED MODULES:
 *   - xcoroutine.h: Executor (pool-allocated execution context),
 *                    XrScopeContext (orthogonal scope policy)
 *   - xblock.c: await helpers read task->state/result
 *   - xworker_exec.c: executor completion writes task->result, recycles executor
 *   - linked go syntax
 */

#ifndef XTASK_H
#define XTASK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include "../runtime/mem/xobj_header.h"
#include "../runtime/value/xvalue.h"
#include "xwait_state.h"

/* ========== Forward Declarations ========== */

struct XrCoroutine;
struct XrArray;
struct XrVMRuntime;
struct XrChannel;
struct XrRuntime;
struct XrRuntimeCore;

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
    XR_TASK_ACTIVE = 0,      // executor running
    XR_TASK_COMPLETING = 1,  // self done, waiting for children
    XR_TASK_CANCELLING = 2,  // cancel requested, children still running
    XR_TASK_COMPLETED = 3,   // final: success
    XR_TASK_FAILED = 4,      // final: error
    XR_TASK_CANCELLED = 5,   // final: cancelled
} XrTaskState;

/* ========== Link Mode (go prefix modifier) ========== */

typedef enum {
    XR_LINK_NONE = 0,    // go fn()        — independent (default)
    XR_LINK_LINKED = 1,  // linked go fn() — bidirectional error propagation
} XrLinkMode;

typedef enum {
    XR_TASK_PAYLOAD_NONE = 0,
    XR_TASK_PAYLOAD_TRANSFER = 1,
    XR_TASK_PAYLOAD_SHARED = 2,
} XrTaskPayloadOwner;

/* ========== Scope Mode (scope prefix modifier) ========== */

typedef enum {
    XR_SCOPE_WAIT = 0,    // scope { } — wait barrier, siblings independent (default)
    XR_SCOPE_LINKED = 1,  // linked scope { } — child fail cancels all + throws
} XrScopeMode;

/* ========== Task Flags ========== */

#define XR_TASK_FLG_HAS_PARENT (1 << 0)         // attached to a parent task
#define XR_TASK_FLG_RUNTIME_OWNED (1 << 1)      // handle lives outside executor heap
#define XR_TASK_FLG_ONE_SHOT_AWAIT (1 << 2)     // compiler-proven single await consumer
#define XR_TASK_FLG_DEFERRED_REGISTRY (1 << 3)  // registry link delayed until batch submit
#define XR_TASK_FLG_RESULT_COPY_SHARED                                                             \
    (1 << 4)  // compiler-planned shared copy for pointer-backed Copy result
/* The failure in `error` arrived on the value-return channel (user
 * `throw <enum>`), not the panic channel.  An error crossing an execution
 * boundary keeps the channel it was raised on: an awaiter re-raises a value
 * error as a value error, so the same `catch (e)` that would have caught it
 * inside one coroutine still catches it across two. */
#define XR_TASK_FLG_ERROR_IS_VALUE (1 << 5)

/* ========== Completion Listener ========== */

typedef enum {
    XR_COMPLETION_WAKE = 0,     // resume a blocked coroutine
    XR_COMPLETION_CHANNEL = 1,  // send event to Channel (for monitor)
    XR_COMPLETION_CLOSURE = 2,  // call user closure (onComplete)
} XrCompletionType;

typedef struct XrCompletionNode {
    struct XrCompletionNode *next;
    uint8_t type;  // XrCompletionType
    uint8_t _pad[7];
    union {
        struct XrCoroutine *waiter;  // WAKE
        struct XrChannel *channel;   // CHANNEL
        XrValue closure;             // CLOSURE (16B)
    } as;
} XrCompletionNode;

/* ========== Task Link (bidirectional association) ========== */

typedef struct XrTaskLink {
    struct XrTask *peer;      // linked peer task
    struct XrTaskLink *next;  // next link in this task's list
} XrTaskLink;

/* ========== XrTask - runtime-managed coroutine handle ========== */

typedef struct XrTask {
    // Object header (must be first field)
    XrObjHeader hdr;  // 16B

    // Cached result — survives executor recycling
    XrValue result;  // 16B
    XrValue error;   // 16B

    // Runtime core that owns Task-independent payload storage/destructors.
    struct XrRuntimeCore *core;  //  8B

    // Back-pointer to executor (NULL after completion + recycle).
    //
    // Doubles as the executor-ownership claim point and the destroy latch:
    // detach paths must claim it with atomic_exchange (only the winner may
    // recycle the executor), and xr_task_runtime_try_destroy_detached
    // refuses to free the task while it is still set — the completing
    // worker keeps it set until it has finished every access to this task,
    // then opens the latch with a release store.
    _Atomic(struct XrCoroutine *) coro;  //  8B

    // State machine + flags
    _Atomic uint8_t state;  //  1B
    uint8_t flags;          //  1B
    uint8_t link_mode;      //  1B: XR_LINK_NONE/LINKED
    /* Set (release) by the completing worker after its last access to this
     * task; one-shot destroy requires it (acquire) so the task is never
     * freed under the completer's feet. */
    _Atomic uint8_t completer_done;  //  1B
    uint8_t result_owner;            //  1B: XrTaskPayloadOwner
    uint8_t error_owner;             //  1B: XrTaskPayloadOwner
    _Atomic uint8_t payload_taken;   //  1B: transferable terminal payload token consumed
    uint16_t child_count;            //  2B

    // Parent-Child hierarchy (only used with linked go / scope)
    _Atomic bool child_lock;      //  1B: spinlock for child list
    struct XrTask *parent;        //  8B
    struct XrTask *first_child;   //  8B
    struct XrTask *next_sibling;  //  8B

    // Bidirectional link peers (task.link() API)
    struct XrTaskLink *links;  //  8B

    // Completion listeners.
    //
    // Accessed concurrently: a producer thread may call
    // xr_task_add_completion while the executor thread is in
    // xr_task_fire_completion. Use atomic ops (Treiber stack push +
    // atomic_exchange drain) so that no node is registered after the
    // list has been drained without being fired. The non-atomic uses
    // (xr_obj_destroy_task, xcoro_heap_traverse) run while the GC has
    // halted mutators and are therefore safe with plain relaxed loads.
    _Atomic(struct XrCompletionNode *) on_completion;  //  8B

    // Await coordination state shared by single and aggregate await paths.
    _Atomic int await_state;  //  4B: NONE / WAITING / RESOLVED

    // Legacy aggregate slot kept for defensive cleanup; active await
    // registrations live in coroutine-owned waiter nodes.
    int waiter_index;            //  4B
    struct XrCoroutine *waiter;  //  8B

    // Single await waiters. Multiple coroutines may await the same Task.
    _Atomic bool await_lock;
    XrTaskAwaitNode *await_waiters;

    // Intrusive runtime registry links. Task handles are owned by XrRuntime,
    // not by executor coroutine heaps. runtime_prev_link points at the list
    // field that references this node, so one-shot destroy can unlink in O(1)
    // under runtime->task_lock instead of scanning the whole task list.
    struct XrTask *runtime_next;
    struct XrTask **runtime_prev_link;
} XrTask;
// ~168B total

/* Heuristic read of the executor pointer (diagnostics, cancel targeting).
 * The executor may be claimed/recycled concurrently; never recycle or
 * destroy through this value — use xr_task_claim_executor for that. */
static inline struct XrCoroutine *xr_task_executor_peek(const XrTask *task) {
    return atomic_load_explicit(&((XrTask *) task)->coro, memory_order_relaxed);
}

/* Claim exclusive ownership of the executor for detach/recycle. Returns the
 * executor if this caller won the claim, NULL if someone else already did
 * (or the completer kept it). acq_rel: acquire pairs with the completer's
 * release latch store; release publishes the claimer's prior writes. */
static inline struct XrCoroutine *xr_task_claim_executor(XrTask *task) {
    return atomic_exchange_explicit(&task->coro, (struct XrCoroutine *) NULL, memory_order_acq_rel);
}

/* Which channel this task's terminal error was raised on.  Only meaningful
 * once the task is FAILED; an awaiter re-raises on the channel reported here. */
static inline bool xr_task_error_is_value(const XrTask *task) {
    return task && (task->flags & XR_TASK_FLG_ERROR_IS_VALUE) != 0;
}

static inline bool xr_task_value_is_transfer_payload(XrValue value) {
    if (XR_IS_STRING(value))
        return xr_value_runtime_string_is_transferable(value);
    return XR_IS_PTR(value) && XR_VALUE_GCPTR(value) && XR_OBJ_IS_TRANSFER(XR_VALUE_GCPTR(value)) &&
           !XR_OBJ_GET_FLAG(XR_VALUE_GCPTR(value), XR_OBJ_TRANSIT);
}

/* ========== Task Lifecycle API ========== */

/* Allocate a runtime-owned task handle.
 * Links task->coro = executor, executor->task = task. */
XR_FUNC struct XrTask *xr_task_create(struct XrRuntime *runtime, struct XrCoroutine *parent_coro,
                                      struct XrCoroutine *executor);
XR_FUNC struct XrTask *xr_task_create_deferred_registry(struct XrRuntime *runtime,
                                                        struct XrCoroutine *parent_coro,
                                                        struct XrCoroutine *executor);
XR_FUNC size_t xr_task_runtime_register_deferred_tasks(struct XrRuntime *runtime,
                                                       struct XrTask **tasks, int count);
XR_FUNC size_t xr_task_runtime_register_deferred_batch(struct XrRuntime *runtime,
                                                       struct XrCoroutine **executors, int count);
XR_FUNC bool xr_task_destroy_deferred_unregistered(struct XrTask *task);
XR_FUNC struct XrTask *xr_task_runtime_detach_all(struct XrRuntime *runtime, size_t *out_count);
XR_FUNC bool xr_task_runtime_try_destroy_detached(struct XrRuntime *runtime, struct XrTask *task);
XR_FUNC void xr_task_destroy_list(struct XrTask *task);
XR_FUNC void xr_task_runtime_destroy_all(struct XrRuntime *runtime);
XR_FUNC void xr_task_isolate_adopt_deferred(struct XrVMRuntime *isolate, struct XrTask *tasks,
                                            size_t count);
XR_FUNC void xr_task_isolate_destroy_deferred(struct XrVMRuntime *isolate);

// Simple state setters (called from xworker.c on executor completion)
XR_FUNC XrValue xr_task_validate_publish_value(struct XrTask *task, XrValue value);
XR_FUNC void xr_task_complete(struct XrTask *task, XrValue result);
XR_FUNC void xr_task_fail(struct XrTask *task, XrValue error, bool error_is_value);
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

// Fail with upward propagation: cancel the parent and every linked peer
XR_FUNC void xr_task_fail_with_propagation(struct XrTask *task, XrValue error, bool error_is_value);

// Fire all completion listeners
XR_FUNC void xr_task_fire_completion(struct XrTask *task);

/* Consume a completed Task result under the await lock. Transfer roots are
 * single-take; const/sync roots retain one observer reference; inline values
 * copy by value. No result path performs an implicit payload copy. */
XR_FUNC XrValue xr_task_consume_result(struct XrRuntimeCore *core, struct XrTask *task,
                                       struct XrCoroutine *dst_coro);
XR_FUNC XrValue xr_task_observe_error(struct XrRuntimeCore *core, struct XrTask *task,
                                      struct XrCoroutine *dst_coro);

static inline bool xr_task_result_was_already_taken(const struct XrTask *task, XrValue value) {
    return task && XR_IS_NULL(value) &&
           atomic_load_explicit(&task->payload_taken, memory_order_acquire) != 0;
}

// Bidirectional link: a fails → cancel b, b fails → cancel a
XR_FUNC void xr_task_link(struct XrTask *a, struct XrTask *b);

// Remove bidirectional link between a and b
XR_FUNC void xr_task_unlink(struct XrTask *a, struct XrTask *b);

// Add a completion listener
XR_FUNC void xr_task_add_completion(struct XrTask *task, struct XrCompletionNode *node);

// Wake the waiter registered on this task (replaces xr_coro_wake_waiter for Task path)
XR_FUNC void xr_task_wake_waiter_runtime(struct XrRuntime *runtime, struct XrTask *task);
XR_FUNC void xr_task_wake_waiter(struct XrVMRuntime *X, struct XrTask *task);

// Clear coroutine-owned await registrations from pending tasks.
XR_FUNC bool xr_task_register_await_node(struct XrTask *task, struct XrCoroutine *waiter,
                                         XrTaskAwaitNode *node, int waiter_index);
XR_FUNC bool xr_task_register_await_waiter(struct XrTask *task, struct XrCoroutine *waiter,
                                           XrAwaitWaitToken *token, int waiter_index);
XR_FUNC void xr_task_unregister_await_waiters(struct XrCoroutine *waiter);
XR_FUNC void xr_task_finish_await_waiters(struct XrCoroutine *waiter);
XR_FUNC void xr_task_cancel_await_waiters(struct XrCoroutine *waiter);

/* ========== Task State Helpers (inline) ========== */

static inline bool xr_task_is_active(const struct XrTask *task) {
    uint8_t s = atomic_load_explicit(&((struct XrTask *) task)->state, memory_order_acquire);
    return s <= XR_TASK_COMPLETING;
}

static inline bool xr_task_is_done(const struct XrTask *task) {
    uint8_t s = atomic_load_explicit(&((struct XrTask *) task)->state, memory_order_acquire);
    return s >= XR_TASK_COMPLETED;
}

static inline bool xr_task_is_cancelled(const struct XrTask *task) {
    uint8_t s = atomic_load_explicit(&((struct XrTask *) task)->state, memory_order_acquire);
    return s == XR_TASK_CANCELLING || s == XR_TASK_CANCELLED;
}

#endif  // XTASK_H
