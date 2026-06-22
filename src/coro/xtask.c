/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtask.c - XrTask lifecycle + structured concurrency implementation
 *
 * KEY CONCEPT:
 *   XrTask is the user-visible handle returned by `go` expressions.
 *   It supports a 6-state machine, parent-child hierarchy for structured
 *   concurrency, and completion listeners for async notification.
 *
 * WHY THIS DESIGN:
 *   - Parent-child + CompletionNode provides the foundation for
 *     linked go / monitored go / scope blocks
 *   - 6-state machine tracks Completing/Cancelling for children wait
 *   - Direct state setters serve executor completion paths; try_complete
 *     and finalize serve structured paths that wait for children
 *
 * RELATED MODULES:
 *   - xtask.h: struct definition + inline helpers
 *   - xcoroutine.h: executor (XrCoroutine)
 *   - xworker_exec.c: calls xr_task_complete on executor finish
 *   - xblock.c: await helpers read task->state/result
 */

#include "xtask.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "xchannel.h"
#include "../runtime/xshared.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/xisolate_internal.h"
#include "../runtime/object/xarray.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <string.h>
#include <stdlib.h>

// Lightweight TAS spinlock for child list serialization.
// Critical sections are very short (list link/unlink), so spinning is fine.
static inline void child_lock_acquire(_Atomic bool *lock) {
    while (atomic_exchange_explicit(lock, true, memory_order_acquire)) {
        // Spin — critical section is < 100 ns
    }
}
static inline void child_lock_release(_Atomic bool *lock) {
    atomic_store_explicit(lock, false, memory_order_release);
}

static inline void await_lock_acquire(_Atomic bool *lock) {
    while (atomic_exchange_explicit(lock, true, memory_order_acquire)) {
    }
}

static inline void await_lock_release(_Atomic bool *lock) {
    atomic_store_explicit(lock, false, memory_order_release);
}

/* ========== Task Creation ========== */

XrTask *xr_task_create(XrRuntime *runtime, XrCoroutine *parent_coro, XrCoroutine *executor) {
    (void) parent_coro;
    XR_DCHECK(runtime != NULL, "xr_task_create: runtime must not be NULL");
    XR_DCHECK(executor != NULL, "xr_task_create: executor must not be NULL");
    if (!runtime || !executor)
        return NULL;

    XrTask *task = (XrTask *) xr_calloc(1, sizeof(XrTask));
    if (!task)
        return NULL;

    xr_obj_header_init_type(&task->gc, XR_TTASK);
    task->gc.objsize = (uint32_t) sizeof(XrTask);

    /* Runtime-managed: Task handles can be observed by scheduler, awaiters,
     * and completion listeners across coroutine boundaries. dup/drop remain
     * no-ops; the runtime registry releases handles at teardown.
     *
     * The MANAGED no-op only takes effect on the cold RC path (rc < 0); a
     * zero-initialized (calloc) refcount is thread-local "unique", so the
     * compiler's hot-path drop would route a Task to xr_coro_gc_rc_free —
     * subtracting bytes the per-coro gc never accounted (the Task lives on the
     * system heap via xr_calloc), underflowing its byte counter. Seat the
     * count in the atomic band so every dup/drop is the intended no-op (same
     * contract as Channel). */
    XR_OBJ_SET_FLAG(&task->gc, XR_OBJ_MANAGED);
    xr_shared_set_refc(&task->gc, 1);

    task->result = xr_null();
    task->error = xr_null();
    atomic_store_explicit(&task->state, XR_TASK_ACTIVE, memory_order_relaxed);
    task->flags = XR_TASK_FLG_RUNTIME_OWNED;
    task->child_count = 0;
    task->link_mode = 0;
    atomic_store_explicit(&task->completer_done, 0, memory_order_relaxed);
    task->_pad2 = 0;
    atomic_init(&task->child_lock, false);
    task->parent = NULL;
    task->first_child = NULL;
    task->next_sibling = NULL;
    task->links = NULL;
    atomic_store_explicit(&task->on_completion, NULL, memory_order_relaxed);
    atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
    task->waiter_index = -1;
    task->waiter = NULL;
    atomic_init(&task->await_lock, false);
    task->await_waiters = NULL;
    task->runtime_next = NULL;

    atomic_store_explicit(&task->coro, executor, memory_order_relaxed);
    executor->task = task;

    xr_mutex_lock(&runtime->task_lock);
    task->runtime_next = runtime->task_list;
    runtime->task_list = task;
    runtime->task_count++;
    xr_mutex_unlock(&runtime->task_lock);

    return task;
}

XrTask *xr_task_runtime_detach_all(XrRuntime *runtime, size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!runtime)
        return NULL;

    xr_mutex_lock(&runtime->task_lock);
    XrTask *task = runtime->task_list;
    size_t count = runtime->task_count;
    runtime->task_list = NULL;
    runtime->task_count = 0;
    xr_mutex_unlock(&runtime->task_lock);

    if (out_count)
        *out_count = count;
    return task;
}

bool xr_task_runtime_try_destroy_detached(XrRuntime *runtime, XrTask *task) {
    if (!runtime || !task || !(task->flags & XR_TASK_FLG_RUNTIME_OWNED))
        return false;

    xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_attempt_count);

    uint8_t state = atomic_load_explicit(&task->state, memory_order_acquire);
    if (state != XR_TASK_COMPLETED) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_fail_state_count);
        return false;
    }
    /* The completing worker still touches the task after publishing
     * COMPLETED (await-waiter wake walk). completer_done (acquire, paired
     * with the completer's release store) is the proof that it finished;
     * freeing before that would pull the task out from under it. */
    if (!atomic_load_explicit(&task->completer_done, memory_order_acquire)) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_fail_coro_count);
        return false;
    }
    if (atomic_load_explicit(&task->coro, memory_order_acquire)) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_fail_coro_count);
        return false;
    }
    if (task->parent || task->first_child || task->links) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_fail_graph_count);
        return false;
    }
    if (atomic_load_explicit(&task->on_completion, memory_order_acquire)) {
        xr_sched_metric_inc(runtime,
                            &runtime->sched_stats.task_one_shot_destroy_fail_listener_count);
        return false;
    }

    await_lock_acquire(&task->await_lock);
    bool has_waiter =
        task->await_waiters != NULL ||
        atomic_load_explicit((_Atomic(XrCoroutine *) *) &task->waiter, memory_order_acquire) !=
            NULL ||
        atomic_load_explicit(&task->await_state, memory_order_acquire) == XR_AWAIT_WAITING;
    await_lock_release(&task->await_lock);
    if (has_waiter) {
        xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_fail_waiter_count);
        return false;
    }

    bool found = false;
    xr_mutex_lock(&runtime->task_lock);
    XrTask **cursor = &runtime->task_list;
    while (*cursor) {
        if (*cursor == task) {
            *cursor = task->runtime_next;
            task->runtime_next = NULL;
            if (runtime->task_count > 0)
                runtime->task_count--;
            found = true;
            break;
        }
        cursor = &(*cursor)->runtime_next;
    }
    xr_mutex_unlock(&runtime->task_lock);
    if (!found) {
        xr_sched_metric_inc(runtime,
                            &runtime->sched_stats.task_one_shot_destroy_fail_unlinked_count);
        return false;
    }

    xr_gc_destroy_task(&task->gc, NULL);
    xr_free(task);
    xr_sched_metric_inc(runtime, &runtime->sched_stats.task_one_shot_destroy_success_count);
    return true;
}

void xr_task_destroy_list(XrTask *task) {
    while (task) {
        XrTask *next = task->runtime_next;
        task->runtime_next = NULL;
        xr_gc_destroy_task(&task->gc, NULL);
        xr_free(task);
        task = next;
    }
}

void xr_task_runtime_destroy_all(XrRuntime *runtime) {
    xr_task_destroy_list(xr_task_runtime_detach_all(runtime, NULL));
}

void xr_task_isolate_adopt_deferred(XrayIsolate *isolate, XrTask *tasks, size_t count) {
    if (!tasks)
        return;
    if (!isolate) {
        xr_task_destroy_list(tasks);
        return;
    }

    size_t actual_count = 0;
    XrTask *tail = tasks;
    while (tail->runtime_next) {
        actual_count++;
        tail = tail->runtime_next;
    }
    actual_count++;
    if (count == 0)
        count = actual_count;

    tail->runtime_next = isolate->deferred_tasks;
    isolate->deferred_tasks = tasks;
    isolate->deferred_task_count += count;
}

void xr_task_isolate_destroy_deferred(XrayIsolate *isolate) {
    if (!isolate || !isolate->deferred_tasks)
        return;
    XrTask *tasks = isolate->deferred_tasks;
    isolate->deferred_tasks = NULL;
    isolate->deferred_task_count = 0;
    xr_task_destroy_list(tasks);
}

/* ========== Simple State Setters ========== */

/*
 * State-transition rules (CAS-protected):
 *
 *   ACTIVE      -> COMPLETING / COMPLETED / CANCELLING / CANCELLED / FAILED
 *   COMPLETING  -> COMPLETED / CANCELLING / CANCELLED / FAILED
 *   CANCELLING  -> CANCELLED              (cancel wins)
 *   COMPLETED / FAILED / CANCELLED        (terminal, immutable)
 *
 * Why CAS: complete/fail and cancel run on different workers (e.g. a slow
 * executor finishes its body just as a linked peer fails on another worker
 * and calls xr_task_cancel_tree on us). Without CAS, the worker that wrote
 * its terminal value last would silently overwrite the other one -- which
 * is exactly the 1136_task_link race where a slow_work that races a failing
 * linked peer ends up reported as COMPLETED with result=10000 instead of
 * CANCELLED. The rule "cancel wins" matches the user-visible semantics of
 * await on a cancelled task (raises cancellation) and awaitResult()
 * (TaskResult.Cancelled).
 */

static bool task_state_is_final(uint8_t s) {
    return s == XR_TASK_COMPLETED || s == XR_TASK_FAILED || s == XR_TASK_CANCELLED;
}

/* CAS state from any value in `from_mask` to `to`. Returns true if this call
 * performed the transition. `from_mask` is a bitmask over (1 << XrTaskState). */
static bool task_cas_state(XrTask *task, uint32_t from_mask, uint8_t to) {
    uint8_t expected;
    do {
        expected = atomic_load_explicit(&task->state, memory_order_acquire);
        if (((1u << expected) & from_mask) == 0)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(&task->state, &expected, to,
                                                    memory_order_acq_rel, memory_order_acquire));
    return true;
}

void xr_task_complete(XrTask *task, XrValue result) {
    if (!task)
        return;
    task->result = result;
    /* ACTIVE/COMPLETING -> COMPLETED. Reject CANCELLING/final: a concurrent
     * cancel from a linked peer already won. */
    if (!task_cas_state(task, (1u << XR_TASK_ACTIVE) | (1u << XR_TASK_COMPLETING),
                        XR_TASK_COMPLETED))
        return;
    xr_task_fire_completion(task);
}

void xr_task_fail(XrTask *task, XrValue error) {
    if (!task)
        return;
    task->error = error;
    if (!task_cas_state(task, (1u << XR_TASK_ACTIVE) | (1u << XR_TASK_COMPLETING), XR_TASK_FAILED))
        return;
    xr_task_fire_completion(task);

    /* Cancel all bidirectionally linked peers on failure. */
    for (XrTaskLink *lk = task->links; lk; lk = lk->next) {
        XrTask *peer = lk->peer;
        if (peer && xr_task_is_active(peer)) {
            xr_task_cancel_tree(peer);
        }
    }
}

void xr_task_cancel(XrTask *task) {
    if (!task)
        return;
    /* Cancel can be invoked from backend cancellation (state ACTIVE/COMPLETING),
     * from cancel_tree's finalize step (state CANCELLING), and from the
     * user task.cancel() API (any non-final state). Reject only final. */
    uint32_t from_mask =
        (1u << XR_TASK_ACTIVE) | (1u << XR_TASK_COMPLETING) | (1u << XR_TASK_CANCELLING);
    if (!task_cas_state(task, from_mask, XR_TASK_CANCELLED))
        return;
    xr_task_fire_completion(task);
}

/* ========== Parent-Child Hierarchy ========== */

void xr_task_attach_child(XrTask *parent, XrTask *child) {
    if (!parent || !child)
        return;
    child->parent = parent;
    child->flags |= XR_TASK_FLG_HAS_PARENT;
    child_lock_acquire(&parent->child_lock);
    child->next_sibling = parent->first_child;
    parent->first_child = child;
    parent->child_count++;
    child_lock_release(&parent->child_lock);
}

void xr_task_detach_child(XrTask *parent, XrTask *child) {
    if (!parent || !child)
        return;
    child_lock_acquire(&parent->child_lock);
    XrTask **pp = &parent->first_child;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next_sibling;
            parent->child_count--;
            child->parent = NULL;
            child->next_sibling = NULL;
            child->flags &= ~XR_TASK_FLG_HAS_PARENT;
            child_lock_release(&parent->child_lock);
            return;
        }
        pp = &(*pp)->next_sibling;
    }
    child_lock_release(&parent->child_lock);
}

/* ========== Structured Completion ========== */

void xr_task_try_complete(XrTask *task, XrValue result) {
    if (!task)
        return;
    task->result = result;

    child_lock_acquire(&task->child_lock);
    bool has_children = (task->first_child != NULL);
    child_lock_release(&task->child_lock);

    if (has_children) {
        /* ACTIVE -> COMPLETING; reject if a concurrent cancel already moved
         * us to CANCELLING/CANCELLED or fail to FAILED. */
        (void) task_cas_state(task, 1u << XR_TASK_ACTIVE, XR_TASK_COMPLETING);
    } else {
        xr_task_finalize(task, XR_TASK_COMPLETED);
    }
}

void xr_task_finalize(XrTask *task, uint8_t final_state) {
    if (!task)
        return;
    /* CAS-reject existing terminal state. final_state itself must be a final
     * one (COMPLETED / FAILED / CANCELLED) — callers (cancel_tree,
     * child_completed, fail_with_propagation) guarantee this. */
    XR_DCHECK(task_state_is_final(final_state), "xr_task_finalize: final_state must be terminal");
    uint8_t expected;
    do {
        expected = atomic_load_explicit(&task->state, memory_order_acquire);
        if (task_state_is_final(expected))
            return;
    } while (!atomic_compare_exchange_weak_explicit(&task->state, &expected, final_state,
                                                    memory_order_acq_rel, memory_order_acquire));

    // Notify parent that this child is done
    if (task->parent) {
        xr_task_child_completed(task->parent, task);
    }

    // Fire completion listeners
    xr_task_fire_completion(task);

    /* Wake any await waiter. Without this, awaits on a task that reaches
     * its terminal state via finalize (cancel_tree's no_children branch,
     * fail_with_propagation's no_children branch, child_completed once the
     * last child reports back) deadlock — the happy path through
     * xr_task_complete / xr_task_fail / xr_task_cancel relies on the worker
     * caller invoking xr_coro_wake_waiter, but cancel propagation via
     * xr_task_cancel_tree runs on a different worker than the cancelled
     * task's executor and there is no such caller. Resolve isolate via the
     * executor coroutine; fall back to current worker if the executor was
     * already detached (we are running on a worker thread either way). */
    XrCoroutine *executor = xr_task_executor_peek(task);
    XrRuntime *runtime = executor ? xr_coro_scheduler(executor) : NULL;
    if (!runtime) {
        XrWorker *w = xr_current_worker();
        if (w && w->p.runtime)
            runtime = w->p.runtime;
    }
    xr_scheduler_host_wake_task_waiter(runtime, task);
}

void xr_task_child_completed(XrTask *parent, XrTask *child) {
    if (!parent || !child)
        return;

    // Detach + empty check must be atomic to avoid TOCTOU.
    // Inline the detach logic here under one lock hold.
    child_lock_acquire(&parent->child_lock);
    XrTask **pp = &parent->first_child;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next_sibling;
            parent->child_count--;
            child->parent = NULL;
            child->next_sibling = NULL;
            child->flags &= ~XR_TASK_FLG_HAS_PARENT;
            break;
        }
        pp = &(*pp)->next_sibling;
    }
    bool no_children = (parent->first_child == NULL);
    child_lock_release(&parent->child_lock);

    if (no_children) {
        uint8_t s = atomic_load_explicit(&parent->state, memory_order_acquire);
        if (s == XR_TASK_COMPLETING) {
            xr_task_finalize(parent, XR_TASK_COMPLETED);
        } else if (s == XR_TASK_CANCELLING) {
            xr_task_finalize(parent, XR_TASK_CANCELLED);
        }
    }
}

/* ========== Cancel Tree ========== */

void xr_task_cancel_tree(XrTask *task) {
    if (!task)
        return;

    uint8_t expected = XR_TASK_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(&task->state, &expected, XR_TASK_CANCELLING,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        if (expected == XR_TASK_COMPLETING) {
            atomic_store_explicit(&task->state, XR_TASK_CANCELLING, memory_order_release);
        } else {
            return;  // already cancelling or done
        }
    }

    // Cancel executor if still running
    XrCoroutine *executor = xr_task_executor_peek(task);
    if (executor) {
        xr_coro_cancel(executor);
    }

    // Recursively cancel all children (hold lock while iterating)
    child_lock_acquire(&task->child_lock);
    for (XrTask *child = task->first_child; child; child = child->next_sibling) {
        xr_task_cancel_tree(child);
    }
    bool no_children = (task->first_child == NULL);
    child_lock_release(&task->child_lock);

    // If no children, finalize now
    if (no_children) {
        xr_task_finalize(task, XR_TASK_CANCELLED);
    }
}

/* ========== Error Propagation ========== */

void xr_task_fail_with_propagation(XrTask *task, XrValue error) {
    if (!task)
        return;
    task->error = error;

    child_lock_acquire(&task->child_lock);
    bool has_children = (task->first_child != NULL);
    if (has_children) {
        /* ACTIVE -> CANCELLING; if a concurrent cancel beat us to it, leave
         * the state alone. We still walk children below to ensure they get
         * cancelled regardless of who flipped the state bit. */
        (void) task_cas_state(task, 1u << XR_TASK_ACTIVE, XR_TASK_CANCELLING);
        for (XrTask *child = task->first_child; child; child = child->next_sibling) {
            xr_task_cancel_tree(child);
        }
        has_children = (task->first_child != NULL);
    }
    child_lock_release(&task->child_lock);

    if (!has_children) {
        xr_task_finalize(task, XR_TASK_FAILED);
    }

    // Propagate to parent (unless parent is supervisor)
    XrTask *p = task->parent;
    if (p && !(p->flags & XR_TASK_FLG_SUPERVISOR)) {
        xr_task_cancel_tree(p);
    }

    // Cancel all bidirectionally linked peers
    for (XrTaskLink *lk = task->links; lk; lk = lk->next) {
        XrTask *peer = lk->peer;
        if (peer && xr_task_is_active(peer)) {
            xr_task_cancel_tree(peer);
        }
    }
}

/* ========== Bidirectional Link API ========== */

static void add_link_entry(XrTask *task, XrTask *peer) {
    XrTaskLink *entry = (XrTaskLink *) xr_calloc(1, sizeof(XrTaskLink));
    if (!entry)
        return;
    entry->peer = peer;
    entry->next = task->links;
    task->links = entry;
}

static void remove_link_entry(XrTask *task, XrTask *peer) {
    XrTaskLink **pp = &task->links;
    while (*pp) {
        if ((*pp)->peer == peer) {
            XrTaskLink *rm = *pp;
            *pp = rm->next;
            xr_free(rm);
            return;
        }
        pp = &(*pp)->next;
    }
}

void xr_task_link(XrTask *a, XrTask *b) {
    if (!a || !b || a == b)
        return;
    add_link_entry(a, b);
    add_link_entry(b, a);

    /* If either task already failed, cancel the other immediately.
     * Handles case where children complete before link() is called. */
    uint8_t sa = atomic_load_explicit(&a->state, memory_order_acquire);
    uint8_t sb = atomic_load_explicit(&b->state, memory_order_acquire);
    if (sa == XR_TASK_FAILED && xr_task_is_active(b)) {
        xr_task_cancel_tree(b);
    } else if (sb == XR_TASK_FAILED && xr_task_is_active(a)) {
        xr_task_cancel_tree(a);
    }
}

void xr_task_unlink(XrTask *a, XrTask *b) {
    if (!a || !b)
        return;
    remove_link_entry(a, b);
    remove_link_entry(b, a);
}

/* ========== Completion Listeners ========== */

void xr_task_add_completion(XrTask *task, XrCompletionNode *node) {
    if (!task || !node)
        return;

    /* Treiber stack push: CAS the head pointer until we succeed. The
     * caller (typically task.monitor() dispatch) already checked
     * xr_task_is_done() and decided to register a listener, but the
     * executor on another worker may transition the task to a terminal
     * state and run xr_task_fire_completion at any point during this
     * push. Re-check after the CAS lands and self-fire if so; the
     * atomic_exchange in fire_completion ensures at most one worker
     * actually drains the list, so the second call from the loser is a
     * no-op. Without this, a node registered after fire_completion has
     * already exchanged the list to NULL would never run — which is
     * exactly the 1132_task_monitor flake on Windows where tryRecv saw
     * an empty buffer because the monitor channel was never notified. */
    XrCompletionNode *head;
    do {
        head = atomic_load_explicit(&task->on_completion, memory_order_acquire);
        node->next = head;
    } while (!atomic_compare_exchange_weak_explicit(&task->on_completion, &head, node,
                                                    memory_order_acq_rel, memory_order_acquire));

    if (xr_task_is_done(task))
        xr_task_fire_completion(task);
}

static void task_ready_waiter(XrRuntime *runtime, XrCoroutine *waiter) {
    if (!waiter)
        return;

    if (!runtime)
        runtime = (XrRuntime *) xr_coro_scheduler(waiter);

    /* xr_coro_ready owns the BLOCKED -> READY claim. Keeping the claim in one
     * place prevents task completion, timeout, and cancellation races from
     * growing independent "if still blocked" enqueue paths. */
    xr_scheduler_ready(runtime, waiter, true);
}

static void task_wake_await_node(XrRuntime *runtime, XrTask *task, XrTaskAwaitNode *node) {
    if (!task || !node)
        return;

    XrCoroutine *waiter = node->waiter;
    int idx = node->waiter_index;
    xr_task_await_node_reset(node);
    if (!waiter)
        return;

    XrCoroWaitState *wait_state = xr_coro_wait_state(waiter);
    switch (idx) {
        case -1: {
            if (wait_state)
                xr_await_wait_token_resolve(&wait_state->await_token);
            task_ready_waiter(runtime, waiter);
            break;
        }
        case -3: {
            if (!wait_state)
                break;
            bool expected = false;
            if (atomic_compare_exchange_strong(&wait_state->any_done, &expected, true)) {
                waiter->result = task->result;
                xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                task_ready_waiter(runtime, waiter);
            }
            atomic_fetch_sub(&wait_state->wait_count, 1);
            break;
        }
        case -4: {
            if (!wait_state)
                break;
            bool is_success = XR_IS_NULL(task->error);
            if (is_success) {
                bool expected = false;
                if (atomic_compare_exchange_strong(&wait_state->any_done, &expected, true)) {
                    waiter->result = task->result;
                    xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                    task_ready_waiter(runtime, waiter);
                }
            }
            int remaining = atomic_fetch_sub(&wait_state->wait_count, 1) - 1;
            if (remaining == 0 && !atomic_load(&wait_state->any_done)) {
                waiter->result = xr_null();
                xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                task_ready_waiter(runtime, waiter);
            }
            break;
        }
        default: {
            if (idx < 0 || !wait_state)
                break;
            int remaining = atomic_fetch_sub(&wait_state->wait_count, 1) - 1;
            if (remaining == 0) {
                xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                task_ready_waiter(runtime, waiter);
            }
            break;
        }
    }
}

void xr_task_wake_waiter_runtime(XrRuntime *runtime, XrTask *task) {
    if (!task)
        return;

    /* Unconditionally mark RESOLVED before checking waiter.
     * If child completes before parent registers await,
     * waiter is NULL but await_state MUST be RESOLVED so
     * await registration's CAS(NONE->WAITING) fails and reads result. */
    int old_await =
        atomic_exchange_explicit(&task->await_state, XR_AWAIT_RESOLVED, memory_order_acq_rel);

    await_lock_acquire(&task->await_lock);
    XrTaskAwaitNode *node = task->await_waiters;
    task->await_waiters = NULL;
    while (node) {
        XrTaskAwaitNode *next = node->next;
        task_wake_await_node(runtime, task, node);
        node = next;
    }
    await_lock_release(&task->await_lock);

    // Atomically claim waiter pointer, prevent duplicate processing
    XrCoroutine *waiter = atomic_exchange_explicit((_Atomic(XrCoroutine *) *) &task->waiter, NULL,
                                                   memory_order_acq_rel);
    if (!waiter)
        return;

    int idx = atomic_load_explicit((_Atomic int *) &task->waiter_index, memory_order_acquire);
    atomic_store_explicit((_Atomic int *) &task->waiter_index, -1, memory_order_relaxed);
    XrCoroWaitState *wait_state = xr_coro_wait_state(waiter);

    switch (idx) {
        case -1: {
            // Single await: wake waiter directly
            if (wait_state)
                xr_await_wait_token_resolve(&wait_state->await_token);
            if (old_await == XR_AWAIT_WAITING) {
                atomic_thread_fence(memory_order_seq_cst);
                task_ready_waiter(runtime, waiter);
            }
            break;
        }
        case -2: {
            /* Scope child completion: decrement count, wake parent when all done.
             * Scope error handling (linked/supervisor) is done in xr_coro_wake_waiter
             * BEFORE delegating here — parent_scope is already cleared at this point. */
            if (!wait_state)
                break;
            int remaining = atomic_fetch_sub(&wait_state->wait_count, 1) - 1;
            if (remaining == 0)
                task_ready_waiter(runtime, waiter);
            break;
        }
        case -3: {
            // await any: wake on first completion
            if (!wait_state)
                break;
            bool expected = false;
            if (atomic_compare_exchange_strong(&wait_state->any_done, &expected, true)) {
                waiter->result = task->result;
                xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                task_ready_waiter(runtime, waiter);
            }
            atomic_fetch_sub(&wait_state->wait_count, 1);
            break;
        }
        case -4: {
            // await anySuccess: wake only on first success
            if (!wait_state)
                break;
            bool is_success = XR_IS_NULL(task->error);
            if (is_success) {
                bool expected = false;
                if (atomic_compare_exchange_strong(&wait_state->any_done, &expected, true)) {
                    waiter->result = task->result;
                    xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                    task_ready_waiter(runtime, waiter);
                }
            }
            int remaining = atomic_fetch_sub(&wait_state->wait_count, 1) - 1;
            if (remaining == 0 && !atomic_load(&wait_state->any_done)) {
                waiter->result = xr_null();
                xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                task_ready_waiter(runtime, waiter);
            }
            break;
        }
        default: {
            // await all (idx >= 0): wake when all children are done.
            if (!wait_state)
                break;
            int remaining = atomic_fetch_sub(&wait_state->wait_count, 1) - 1;
            if (remaining == 0) {
                xr_multi_await_wait_token_resolve(&wait_state->multi_await_token);
                task_ready_waiter(runtime, waiter);
            }
            break;
        }
    }
}

void xr_task_wake_waiter(XrayIsolate *X, XrTask *task) {
    xr_task_wake_waiter_runtime(X ? (XrRuntime *) X->scheduler_runtime : NULL, task);
}

void xr_task_fire_completion(XrTask *task) {
    if (!task)
        return;
    /* Atomic exchange so concurrent fire_completion callers (e.g. the
     * executor finishing the task and a late xr_task_add_completion
     * that detected the terminal state) both run safely — the loser
     * gets a NULL list and drops out of the loop without firing
     * anything twice. */
    XrCompletionNode *node =
        atomic_exchange_explicit(&task->on_completion, NULL, memory_order_acq_rel);

    while (node) {
        XrCompletionNode *next = node->next;
        switch (node->type) {
            case XR_COMPLETION_WAKE:
                if (node->as.waiter) {
                    xr_scheduler_ready((XrRuntime *) xr_coro_scheduler(node->as.waiter),
                                       node->as.waiter, true);
                }
                break;
            case XR_COMPLETION_CHANNEL: {
                /* Monitor notification: send the task itself as event.
                 * Receiver checks task.done/task.result/task.error/task.cancelled
                 * to determine outcome. Zero allocation, full state access.
                 *
                 * Monitor channels are single-shot — the only message they ever
                 * carry is this completion notification. Close immediately after
                 * sending so that any subsequent recv() returns the closed
                 * sentinel instead of blocking forever, and so that the runtime
                 * channel-leak accounting (create vs close counters checked at
                 * xworker.c shutdown) stays balanced. */
                XrChannel *ch = node->as.channel;
                if (ch) {
                    xr_channel_notify_send(ch, xr_value_from_task(task));
                    xr_channel_close(ch);
                }
                break;
            }
            case XR_COMPLETION_CLOSURE:
                // Will be implemented in onComplete API
                break;
        }
        xr_free(node);
        node = next;
    }
}

/* ========== GC Destroy (called by sweep when Task is reclaimed) ========== */

void xr_gc_destroy_task(XrObjHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    XrTask *task = (XrTask *) obj;

    // Free xr_calloc'd bidirectional link entries
    XrTaskLink *lk = task->links;
    while (lk) {
        XrTaskLink *next = lk->next;
        xr_free(lk);
        lk = next;
    }
    task->links = NULL;

    // Free xr_calloc'd completion listeners (unfired, e.g. task cancelled before monitor read).
    // Mutators are halted by the GC at this point, so a relaxed load is sufficient.
    XrCompletionNode *cn = atomic_load_explicit(&task->on_completion, memory_order_relaxed);
    while (cn) {
        XrCompletionNode *next = cn->next;
        xr_free(cn);
        cn = next;
    }
    atomic_store_explicit(&task->on_completion, NULL, memory_order_relaxed);
}
