/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtask_await.c - Task await registration and owner-side cleanup.
 */

#include "xtask.h"
#include "xcoroutine.h"

static inline void task_await_lock_acquire(_Atomic bool *lock) {
    while (atomic_exchange_explicit(lock, true, memory_order_acquire)) {
    }
}

static inline void task_await_lock_release(_Atomic bool *lock) {
    atomic_store_explicit(lock, false, memory_order_release);
}

bool xr_task_register_await_node(XrTask *task, XrCoroutine *waiter, XrTaskAwaitNode *node,
                                 int waiter_index) {
    if (!task || !waiter || !node)
        return false;

    xr_task_await_node_reset(node);
    node->task = task;
    node->waiter = waiter;
    node->waiter_index = waiter_index;

    task_await_lock_acquire(&task->await_lock);
    if (xr_task_is_done(task)) {
        xr_task_await_node_reset(node);
        task_await_lock_release(&task->await_lock);
        return false;
    }

    atomic_store_explicit(&task->await_state, XR_AWAIT_WAITING, memory_order_release);
    node->next = task->await_waiters;
    node->linked = true;
    task->await_waiters = node;
    task_await_lock_release(&task->await_lock);
    return true;
}

bool xr_task_register_await_waiter(XrTask *task, XrCoroutine *waiter, XrAwaitWaitToken *token,
                                   int waiter_index) {
    if (!token)
        return false;
    return xr_task_register_await_node(task, waiter, &token->node, waiter_index);
}

static void task_clear_waiter_if_matches(XrTask *task, XrCoroutine *waiter) {
    if (!task || !waiter)
        return;

    XrCoroutine *expected = waiter;
    task_await_lock_acquire(&task->await_lock);
    if (atomic_compare_exchange_strong_explicit((_Atomic(XrCoroutine *) *) &task->waiter, &expected,
                                                NULL, memory_order_acq_rel, memory_order_acquire)) {
        atomic_store_explicit((_Atomic int *) &task->waiter_index, -1, memory_order_relaxed);
        if (!task->await_waiters) {
            int waiting = XR_AWAIT_WAITING;
            (void) atomic_compare_exchange_strong_explicit(&task->await_state, &waiting,
                                                           XR_AWAIT_NONE, memory_order_acq_rel,
                                                           memory_order_acquire);
        }
    }
    task_await_lock_release(&task->await_lock);
}

static void task_unregister_await_node(XrTask *task, XrTaskAwaitNode *node) {
    if (!node)
        return;
    if (!task)
        task = node->task;
    if (!task) {
        xr_task_await_node_reset(node);
        return;
    }

    task_await_lock_acquire(&task->await_lock);
    if (node->linked) {
        XrTaskAwaitNode **pp = &task->await_waiters;
        while (*pp) {
            if (*pp == node) {
                *pp = node->next;
                break;
            }
            pp = &(*pp)->next;
        }
    }
    xr_task_await_node_reset(node);
    XrCoroutine *legacy_waiter =
        atomic_load_explicit((_Atomic(XrCoroutine *) *) &task->waiter, memory_order_acquire);
    if (!task->await_waiters && legacy_waiter == NULL) {
        int waiting = XR_AWAIT_WAITING;
        (void) atomic_compare_exchange_strong_explicit(&task->await_state, &waiting, XR_AWAIT_NONE,
                                                       memory_order_acq_rel, memory_order_acquire);
    }
    task_await_lock_release(&task->await_lock);
}

static void task_unregister_single_await_waiter(XrTask *task, XrCoroutine *waiter,
                                                XrAwaitWaitToken *token) {
    if (!task || !waiter || !token)
        return;
    /* Unlocked pre-check (relaxed): the wake walk may be resetting the node
     * under the task's await_lock concurrently; the locked unregister
     * re-validates before unlinking. */
    if (xr_task_await_node_waiter(&token->node) == waiter ||
        xr_task_await_node_linked(&token->node))
        task_unregister_await_node(task, &token->node);
}

static void task_unregister_multi_await_waiters(XrCoroutine *waiter, XrCoroWaitState *wait_state) {
    XrMultiAwaitWaitToken *token = &wait_state->multi_await_token;
    (void) atomic_exchange_explicit(&token->tasks, NULL, memory_order_acq_rel);
    XrTaskAwaitNode *nodes = token->nodes;
    int node_count = token->node_count;
    for (int i = 0; nodes && i < node_count; i++) {
        if (xr_task_await_node_waiter(&nodes[i]) == waiter || xr_task_await_node_linked(&nodes[i]))
            task_unregister_await_node(nodes[i].task, &nodes[i]);
    }
}

void xr_task_unregister_await_waiters(XrCoroutine *waiter) {
    XrCoroWaitState *wait_state = xr_coro_wait_state(waiter);
    if (!wait_state)
        return;

    XrTask *single = atomic_exchange_explicit(&wait_state->await_task, NULL, memory_order_acq_rel);
    if (single) {
        task_unregister_single_await_waiter(single, waiter, &wait_state->await_token);
        task_clear_waiter_if_matches(single, waiter);
    }

    task_unregister_multi_await_waiters(waiter, wait_state);
}

void xr_task_finish_await_waiters(XrCoroutine *waiter) {
    XrCoroWaitState *wait_state = xr_coro_wait_state(waiter);
    if (!wait_state)
        return;

    xr_task_unregister_await_waiters(waiter);
    atomic_store_explicit(&wait_state->wait_count, 0, memory_order_relaxed);
    atomic_store_explicit(&wait_state->any_done, false, memory_order_relaxed);
    xr_await_wait_token_finish(&wait_state->await_token);
    xr_multi_await_wait_token_finish(&wait_state->multi_await_token);
}

void xr_task_cancel_await_waiters(XrCoroutine *waiter) {
    XrCoroWaitState *wait_state = xr_coro_wait_state(waiter);
    if (!wait_state)
        return;

    xr_task_unregister_await_waiters(waiter);
    atomic_store_explicit(&wait_state->wait_count, 0, memory_order_relaxed);
    atomic_store_explicit(&wait_state->any_done, false, memory_order_relaxed);
    xr_await_wait_token_cancel(&wait_state->await_token);
    xr_multi_await_wait_token_cancel(&wait_state->multi_await_token);
}
