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
 *   - Simple state setters (complete/fail/cancel) remain for backward compat
 *     with current xworker.c paths; try_complete/finalize for structured paths
 *
 * RELATED MODULES:
 *   - xtask.h: struct definition + inline helpers
 *   - xcoroutine.h: executor (XrCoroutine)
 *   - xworker.c: calls xr_task_complete on executor finish
 *   - xvm_cold_paths.c: vm_await reads task->state/result
 */

#include "xtask.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "xchannel.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
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

/* ========== Task Creation ========== */

XrTask *xr_task_create(XrCoroutine *parent_coro, XrCoroutine *executor) {
    XR_DCHECK(executor != NULL, "xr_task_create: executor must not be NULL");
    (void) parent_coro;

    /* Allocate on executor's heap: executor keeps task alive via coro->task
     * (GC-marked in mark_coro_roots). The OP_AWAIT paths must NOT recycle
     * the executor while parent still references the task — parent's GC
     * would scan freed Immix memory (use-after-free). */
    XrTask *task = (XrTask *) xr_alloc(executor, sizeof(XrTask), XR_TTASK);
    if (!task)
        return NULL;

    task->result = xr_null();
    task->error = xr_null();
    task->coro = executor;
    atomic_store_explicit(&task->state, XR_TASK_ACTIVE, memory_order_relaxed);
    task->flags = 0;
    task->child_count = 0;
    task->link_mode = 0;
    task->_pad1 = 0;
    task->_pad2 = 0;
    atomic_init(&task->child_lock, false);
    task->parent = NULL;
    task->first_child = NULL;
    task->next_sibling = NULL;
    task->links = NULL;
    task->on_completion = NULL;
    atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
    task->waiter_index = -1;
    task->waiter = NULL;

    executor->task = task;
    return task;
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
 * its terminal value last would silently overwrite the other one — which
 * is exactly the 1136_task_link race where a slow_work that races a failing
 * linked peer ends up reported as COMPLETED with result=10000 instead of
 * CANCELLED. The rule "cancel wins" matches the user-visible semantics of
 * await on a cancelled task (returns null).
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
                                                   memory_order_acq_rel,
                                                   memory_order_acquire));
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
    if (!task_cas_state(task, (1u << XR_TASK_ACTIVE) | (1u << XR_TASK_COMPLETING),
                        XR_TASK_FAILED))
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
    /* Cancel can be invoked from XR_VM_CANCELLED (state ACTIVE/COMPLETING),
     * from cancel_tree's finalize step (state CANCELLING), and from the
     * user task.cancel() API (any non-final state). Reject only final. */
    uint32_t from_mask = (1u << XR_TASK_ACTIVE) | (1u << XR_TASK_COMPLETING) |
                         (1u << XR_TASK_CANCELLING);
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
                                                   memory_order_acq_rel,
                                                   memory_order_acquire));

    // Notify parent that this child is done
    if (task->parent) {
        xr_task_child_completed(task->parent, task);
    }

    // Fire completion listeners
    xr_task_fire_completion(task);

    /* Wake any vm_await waiter. Without this, awaits on a task that reaches
     * its terminal state via finalize (cancel_tree's no_children branch,
     * fail_with_propagation's no_children branch, child_completed once the
     * last child reports back) deadlock — the happy path through
     * xr_task_complete / xr_task_fail / xr_task_cancel relies on the worker
     * caller invoking xr_coro_wake_waiter, but cancel propagation via
     * xr_task_cancel_tree runs on a different worker than the cancelled
     * task's executor and there is no such caller. Resolve isolate via the
     * executor coroutine; fall back to current worker if the executor was
     * already detached (we are running on a worker thread either way). */
    XrayIsolate *X = NULL;
    if (task->coro)
        X = task->coro->isolate;
    if (!X) {
        XrWorker *w = xr_current_worker();
        if (w && w->p.runtime)
            X = w->p.runtime->isolate;
    }
    if (X)
        xr_task_wake_waiter(X, task);
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
    if (task->coro) {
        xr_coro_cancel(task->coro);
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
    node->next = task->on_completion;
    task->on_completion = node;
}

/* ========== Await Wake (replaces xr_coro_wake_waiter for Task path) ========== */

void xr_task_wake_waiter(XrayIsolate *X, XrTask *task) {
    if (!X || !task)
        return;

    /* Unconditionally mark RESOLVED before checking waiter.
     * If child completes before parent calls vm_await,
     * waiter is NULL but await_state MUST be RESOLVED so
     * vm_await's CAS(NONE->WAITING) fails and reads result. */
    int old_await =
        atomic_exchange_explicit(&task->await_state, XR_AWAIT_RESOLVED, memory_order_acq_rel);

    // Atomically claim waiter pointer, prevent duplicate processing
    XrCoroutine *waiter = atomic_exchange_explicit((_Atomic(XrCoroutine *) *) &task->waiter, NULL,
                                                   memory_order_acq_rel);
    if (!waiter)
        return;

    int idx = atomic_load_explicit((_Atomic int *) &task->waiter_index, memory_order_acquire);
    atomic_store_explicit((_Atomic int *) &task->waiter_index, -1, memory_order_relaxed);

    switch (idx) {
        case -1: {
            // Single await: wake waiter directly
            if (old_await == XR_AWAIT_WAITING) {
                atomic_thread_fence(memory_order_seq_cst);
                if (xr_coro_flags_has(waiter, XR_CORO_FLG_BLOCKED)) {
                    xr_coro_ready(X, waiter, true);
                }
            }
            break;
        }
        case -2: {
            /* Scope child completion: decrement count, wake parent when all done.
             * Scope error handling (linked/supervisor) is done in xr_coro_wake_waiter
             * BEFORE delegating here — parent_scope is already cleared at this point. */
            int remaining = atomic_fetch_sub(&waiter->wait_count, 1) - 1;
            if (remaining == 0 && xr_coro_flags_has(waiter, XR_CORO_FLG_BLOCKED)) {
                xr_coro_ready(X, waiter, true);
            }
            break;
        }
        case -3: {
            // await any: wake on first completion
            bool expected = false;
            if (atomic_compare_exchange_strong(&waiter->any_done, &expected, true)) {
                waiter->result = task->result;
                if (xr_coro_flags_has(waiter, XR_CORO_FLG_BLOCKED)) {
                    xr_coro_ready(X, waiter, true);
                }
            }
            atomic_fetch_sub(&waiter->wait_count, 1);
            break;
        }
        case -4: {
            // await anySuccess: wake only on first success
            bool is_success = !XR_IS_STRING(task->error);
            if (is_success) {
                bool expected = false;
                if (atomic_compare_exchange_strong(&waiter->any_done, &expected, true)) {
                    waiter->result = task->result;
                    if (xr_coro_flags_has(waiter, XR_CORO_FLG_BLOCKED)) {
                        xr_coro_ready(X, waiter, true);
                    }
                }
            }
            int remaining = atomic_fetch_sub(&waiter->wait_count, 1) - 1;
            if (remaining == 0 && !atomic_load(&waiter->any_done)) {
                waiter->result = xr_null();
                if (xr_coro_flags_has(waiter, XR_CORO_FLG_BLOCKED)) {
                    xr_coro_ready(X, waiter, true);
                }
            }
            break;
        }
        default:
            // await all (idx >= 0): store result at index, wake when all done
            if (waiter->await_results && idx < waiter->await_results->length) {
                xr_array_set_direct(waiter->await_results, idx, task->result);
            }
            int remaining = atomic_fetch_sub(&waiter->wait_count, 1) - 1;
            if (remaining == 0 && xr_coro_flags_has(waiter, XR_CORO_FLG_BLOCKED)) {
                xr_coro_ready(X, waiter, true);
            }
            break;
    }
}

void xr_task_fire_completion(XrTask *task) {
    if (!task)
        return;
    XrCompletionNode *node = task->on_completion;
    task->on_completion = NULL;

    while (node) {
        XrCompletionNode *next = node->next;
        switch (node->type) {
            case XR_COMPLETION_WAKE:
                if (node->as.waiter && node->as.waiter->isolate) {
                    xr_coro_ready(node->as.waiter->isolate, node->as.waiter, true);
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

void xr_gc_destroy_task(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
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

    // Free xr_calloc'd completion listeners (unfired, e.g. task cancelled before monitor read)
    XrCompletionNode *cn = task->on_completion;
    while (cn) {
        XrCompletionNode *next = cn->next;
        xr_free(cn);
        cn = next;
    }
    task->on_completion = NULL;
}
