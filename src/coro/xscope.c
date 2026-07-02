/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xscope.c - Scope membership and scope completion wake handling.
 */

#include "xcoroutine.h"
#include "xtask.h"
#include "xworker.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/xisolate_internal.h"

XrScopeContext *xr_coro_parent_scope(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->parent_scope : NULL;
}

bool xr_coro_set_parent_scope(XrCoroutine *coro, XrScopeContext *scope) {
    if (!coro)
        return false;
    if (!scope) {
        if (coro->ext)
            coro->ext->parent_scope = NULL;
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->parent_scope = scope;
    return true;
}

XrCoroutine *xr_coro_scope_sibling(const XrCoroutine *coro) {
    return (coro && coro->ext) ? coro->ext->scope_sibling : NULL;
}

bool xr_coro_set_scope_sibling(XrCoroutine *coro, XrCoroutine *sibling) {
    if (!coro)
        return false;
    if (!sibling) {
        if (coro->ext)
            coro->ext->scope_sibling = NULL;
        return true;
    }
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;
    ext->scope_sibling = sibling;
    return true;
}

void xr_coro_detach_scope_child(XrCoroutine *coro) {
    XrScopeContext *scope = xr_coro_parent_scope(coro);
    if (!scope)
        return;

    while (atomic_exchange_explicit(&scope->child_lock, true, memory_order_acquire)) {
    }
    XrCoroutine *prev = NULL;
    XrCoroutine *cur = scope->first_child;
    bool removed = false;
    while (cur) {
        XrCoroutine *next = xr_coro_scope_sibling(cur);
        if (cur == coro) {
            if (prev) {
                (void) xr_coro_set_scope_sibling(prev, next);
            } else {
                scope->first_child = next;
            }
            (void) xr_coro_set_scope_sibling(coro, NULL);
            removed = true;
            break;
        }
        prev = cur;
        cur = next;
    }
    atomic_store_explicit(&scope->child_lock, false, memory_order_release);
    if (removed)
        atomic_fetch_sub_explicit(&scope->count, 1, memory_order_relaxed);
    (void) xr_coro_set_parent_scope(coro, NULL);
}

// Add coroutine to current scope.
//
// Per-coroutine scope tracking: prefer parent->current_scope, fallback to
// runtime/sched globals for main thread.
void xr_scope_add_coro(XrCoroState *sched, XrCoroutine *coro, XrCoroutine *parent) {
    if (!coro)
        return;

    XrScopeContext *scope = NULL;

    if (parent)
        scope = atomic_load_explicit(&parent->current_scope, memory_order_relaxed);

    if (!scope) {
        XrWorker *worker = xr_current_worker();
        if (worker && worker->p.runtime)
            scope = worker->p.runtime->current_scope;
    }

    if (!scope && sched)
        scope = sched->current_scope;

    if (!scope)
        return;

    if (!xr_coro_set_parent_scope(coro, scope))
        return;
    atomic_fetch_add(&scope->count, 1);
}

/* ========== Scope Child List Lock Helpers ==========
 *
 * scope->child_lock is a spin-lock guarding:
 *
 *   - scope->first_child / child's scope sibling;
 *   - scope->cancel_requested;
 *   - child parent-scope clearing during sibling cancel.
 *
 * Hold time is bounded to one list mutation or a small sibling walk. */
static inline void scope_lock_acquire(XrScopeContext *scope) {
    while (atomic_exchange_explicit(&scope->child_lock, true, memory_order_acquire)) {
    }
}

static inline void scope_lock_release(XrScopeContext *scope) {
    atomic_store_explicit(&scope->child_lock, false, memory_order_release);
}

/* Record this child's terminal state and update scope->first_error /
 * scope->outcomes per policy mode.  Returns true when the child failed, so
 * linked scope can cancel siblings. Caller must hold scope->child_lock. */
static bool wake_waiter_record_child_completion_locked(XrCoroutine *coro, XrScopeContext *scope) {
    if (scope->mode == XR_SCOPE_WAIT)
        return false;

    const XrScopeTransferOps *ops = xr_runtime_core_scope_transfer_ops(coro->core);
    if (ops && ops->record_child_completion_locked)
        return ops->record_child_completion_locked(coro, scope);

    XrValue err = coro->error;
    if (XR_IS_NULL(err) && coro->task)
        err = coro->task->error;
    if (XR_IS_NULL(err))
        return false;

    if (scope->mode == XR_SCOPE_LINKED && XR_IS_NULL(scope->first_error)) {
        scope->first_error = err;
        scope->first_error_is_value = coro->error_is_value;
    }
    return true;
}

static void wake_waiter_unlink_from_scope_locked(XrCoroutine *coro, XrScopeContext *scope) {
    XrCoroutine *prev = NULL;
    XrCoroutine *cur = scope->first_child;
    while (cur) {
        XrCoroutine *next = xr_coro_scope_sibling(cur);
        if (cur == coro) {
            if (prev) {
                (void) xr_coro_set_scope_sibling(prev, next);
            } else {
                scope->first_child = next;
            }
            xr_coro_set_scope_sibling(coro, NULL);
            return;
        }
        prev = cur;
        cur = next;
    }
}

static void wake_waiter_cancel_linked_siblings_locked(XrScopeContext *scope) {
    for (XrCoroutine *sib = scope->first_child; sib; sib = xr_coro_scope_sibling(sib)) {
        if (xr_coro_flags_has(sib, XR_CORO_FLG_DONE))
            continue;
        xr_coro_flags_set(sib, XR_CORO_FLG_CANCEL_REQUESTED);
        xr_coro_request_yield(sib);
        xr_scheduler_ready((XrRuntime *) xr_coro_scheduler(sib), sib, false);
    }
}

static bool wake_waiter_scope_owner_ready(const XrScopeContext *scope, const XrCoroutine *owner) {
    if (!scope || !owner ||
        atomic_load_explicit(&owner->current_scope, memory_order_acquire) != scope)
        return false;
    if (xr_coro_get_wait_reason(xr_coro_flags_load(owner)) !=
        (XR_CORO_WAIT_SCOPE >> XR_CORO_WAIT_SHIFT)) {
        return false;
    }
    return xr_coro_flags_has(owner, XR_CORO_FLG_BLOCKED);
}

static void wake_waiter_finish_scope_completion(XrCoroutine *coro, XrScopeContext *scope) {
    XrCoroutine *owner = scope->owner;
    bool owner_waiting_scope = wake_waiter_scope_owner_ready(scope, owner);
    int remaining = atomic_fetch_sub(&scope->count, 1) - 1;
    xr_coro_set_parent_scope(coro, NULL);
    if (remaining == 0 && owner_waiting_scope) {
        XrCoroWaitState *wait = xr_coro_wait_state(owner);
        if (wait)
            xr_scope_wait_token_resolve(&wait->scope_token);
        xr_scheduler_ready((XrRuntime *) xr_coro_scheduler(owner), owner, true);
    }
}

static void wake_waiter_handle_scope_completion(XrCoroutine *coro, XrScopeContext *scope) {
    scope_lock_acquire(scope);
    bool child_failed = wake_waiter_record_child_completion_locked(coro, scope);
    wake_waiter_unlink_from_scope_locked(coro, scope);
    if (child_failed && scope->mode == XR_SCOPE_LINKED &&
        !atomic_exchange(&scope->cancel_requested, true)) {
        wake_waiter_cancel_linked_siblings_locked(scope);
    }
    scope_lock_release(scope);

    wake_waiter_finish_scope_completion(coro, scope);
}

static void wake_waiter_notify_task(XrRuntime *runtime, XrCoroutine *coro) {
    if (coro->task)
        xr_task_wake_waiter_runtime(runtime, coro->task);
}

void xr_coro_wake_scope_waiter(XrVMRuntime *X, XrCoroutine *coro) {
    (void) X;
    if (!coro)
        return;

    XrScopeContext *scope = xr_coro_parent_scope(coro);
    if (scope)
        wake_waiter_handle_scope_completion(coro, scope);
}

void xr_coro_wake_scope_waiter_runtime(XrRuntime *runtime, XrCoroutine *coro) {
    (void) runtime;
    xr_coro_wake_scope_waiter(NULL, coro);
}

void xr_coro_wake_waiter_runtime(XrRuntime *runtime, XrCoroutine *coro) {
    if (!coro)
        return;

    xr_coro_wake_scope_waiter_runtime(runtime, coro);
    wake_waiter_notify_task(runtime, coro);
}

void xr_coro_wake_waiter(XrVMRuntime *X, XrCoroutine *coro) {
    if (!X || !coro)
        return;

    xr_coro_wake_waiter_runtime((XrRuntime *) X->vm.scheduler, coro);
}
