/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemaphore.c - Shared counting semaphore primitive
 */

#include "xsemaphore.h"

#include <stddef.h>
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xsystem_heap.h"
#include "xblock.h"
#include "xcoroutine.h"
#include "xsemaphore_wait.h"
#include "xworker.h"
#include "xyieldable.h"

static XrRuntime *semaphore_runtime(XrSemaphore *sem) {
    return sem ? (XrRuntime *) sem->scheduler : NULL;
}

static bool semaphore_runtime_can_wake(XrRuntime *runtime) {
    return runtime && runtime->workers && runtime->worker_count > 0;
}

static bool semaphore_waiter_enqueue_locked(XrSemaphore *sem, XrCoroutine *coro) {
    XR_DCHECK(sem != NULL, "semaphore_waiter_enqueue_locked: NULL sem");
    XR_DCHECK(coro != NULL, "semaphore_waiter_enqueue_locked: NULL coro");
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;

    ext->wait_link = NULL;
    ext->wait_prev = sem->wait_last;
    if (sem->wait_last) {
        sem->wait_last->ext->wait_link = coro;
    } else {
        sem->wait_first = coro;
    }
    sem->wait_last = coro;
    atomic_fetch_add_explicit(&sem->waiter_count, 1, memory_order_relaxed);
    return true;
}

static XrCoroutine *semaphore_waiter_pop_locked(XrSemaphore *sem) {
    XR_DCHECK(sem != NULL, "semaphore_waiter_pop_locked: NULL sem");
    XrCoroutine *coro = sem->wait_first;
    if (!coro)
        return NULL;
    xr_semaphore_waiter_unlink_locked(sem, coro);
    return coro;
}

typedef struct XrSemaphoreWakeBatch {
    XrCoroutine *first;
    XrCoroutine *last;
    int count;
} XrSemaphoreWakeBatch;

enum {
    XR_SEMAPHORE_STACK_WAKE_BATCHES = 64
};

static void semaphore_wake_batch_append(XrSemaphoreWakeBatch *batch, XrCoroutine *coro) {
    if (!batch || !coro)
        return;
    coro->sched_link = NULL;
    if (batch->last) {
        batch->last->sched_link = coro;
    } else {
        batch->first = coro;
    }
    batch->last = coro;
    batch->count++;
}

static bool semaphore_claim_waiter(XrSemaphore *sem, XrCoroutine *coro, XrRuntime *runtime,
                                   bool success, int *target_id_out) {
    if (!semaphore_runtime_can_wake(runtime) || !coro || !target_id_out)
        return false;
    if (!xr_coro_claim_wake(coro))
        return false;

    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (wait)
        xr_semaphore_wait_token_resolve(&wait->semaphore_token, success);
    xr_coro_resume_store(coro, XR_RESUME_OK);
    int target_id = xr_coro_wake_target_id(coro);
    if (target_id < 0 || target_id >= runtime->worker_count)
        target_id = 0;
    *target_id_out = target_id;
    (void) sem;
    return true;
}

static int64_t semaphore_wake_list(XrSemaphore *sem, XrCoroutine *list, bool success) {
    if (!sem || !list)
        return 0;
    XrRuntime *runtime = semaphore_runtime(sem);
    if (!semaphore_runtime_can_wake(runtime))
        return 0;

    XrWorker *current = xr_current_worker();
    bool current_matches = current && current->p.runtime == runtime;
    XrSemaphoreWakeBatch local_batch = {0};
    XrSemaphoreWakeBatch stack_batches[XR_SEMAPHORE_STACK_WAKE_BATCHES];
    XrSemaphoreWakeBatch *remote_batches = NULL;
    bool remote_batches_heap = false;
    if (runtime->worker_count > 0) {
        size_t batch_count = (size_t) runtime->worker_count;
        if (batch_count <= XR_SEMAPHORE_STACK_WAKE_BATCHES) {
            memset(stack_batches, 0, batch_count * sizeof(XrSemaphoreWakeBatch));
            remote_batches = stack_batches;
        } else {
            remote_batches =
                (XrSemaphoreWakeBatch *) xr_calloc(batch_count, sizeof(XrSemaphoreWakeBatch));
            remote_batches_heap = remote_batches != NULL;
        }
    }
    int remote_ready_count = 0;
    int64_t failed_claims = 0;

    while (list) {
        XrCoroutine *next = list->sched_link;
        list->sched_link = NULL;
        int target_id = 0;
        if (semaphore_claim_waiter(sem, list, runtime, success, &target_id)) {
            if (current_matches && target_id == current->p.id) {
                semaphore_wake_batch_append(&local_batch, list);
            } else if (remote_batches) {
                semaphore_wake_batch_append(&remote_batches[target_id], list);
                remote_ready_count++;
            } else {
                xr_worker_inbox_enqueue(runtime, target_id, list);
                remote_ready_count++;
            }
        } else if (success) {
            failed_claims++;
        }
        list = next;
    }

    if (local_batch.first) {
        (void) xr_worker_push_lifo_batch(current, local_batch.first);
        xr_runtime_wake_idle_worker(runtime);
    }
    if (remote_batches) {
        for (int i = 0; i < runtime->worker_count; i++) {
            XrSemaphoreWakeBatch *batch = &remote_batches[i];
            if (batch->first)
                xr_worker_inbox_enqueue_batch(runtime, i, batch->first, batch->last, batch->count);
        }
        if (remote_batches_heap)
            xr_free(remote_batches);
    }
    if (remote_ready_count > 0 &&
        atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed) == 0) {
        xr_runtime_wake_idle_worker(runtime);
    }
    return failed_claims;
}

XrSemaphore *xr_semaphore_new(XrRuntimeCore *core, XrRuntime *scheduler, int64_t permits) {
    if (!core || !core->sys_heap)
        return NULL;
    if (permits < 0)
        permits = 0;
    XrSemaphore *sem =
        (XrSemaphore *) xr_sysheap_alloc_shared(core->sys_heap, sizeof(XrSemaphore), XR_TSEMAPHORE);
    if (!sem)
        return NULL;
    xr_amutex_init(&sem->lock);
    atomic_store_explicit(&sem->available, permits, memory_order_relaxed);
    atomic_store_explicit(&sem->waiter_count, 0, memory_order_relaxed);
    atomic_store_explicit(&sem->closed, false, memory_order_relaxed);
    sem->wait_first = NULL;
    sem->wait_last = NULL;
    sem->core = core;
    sem->scheduler = scheduler;
    return sem;
}

bool xr_semaphore_try_acquire(XrSemaphore *sem) {
    if (!sem || atomic_load_explicit(&sem->closed, memory_order_acquire))
        return false;
    int64_t old = atomic_load_explicit(&sem->available, memory_order_acquire);
    while (old > 0) {
        if (atomic_compare_exchange_weak_explicit(&sem->available, &old, old - 1,
                                                  memory_order_acq_rel, memory_order_acquire))
            return true;
    }
    return false;
}

int64_t xr_semaphore_release(XrSemaphore *sem, int64_t count) {
    if (!sem)
        return 0;
    if (count <= 0)
        count = 1;

    XrCoroutine *list = NULL;
    XrCoroutine *last = NULL;
    int64_t remaining = count;

    xr_amutex_lock(&sem->lock);
    if (atomic_load_explicit(&sem->closed, memory_order_acquire)) {
        int64_t available = atomic_load_explicit(&sem->available, memory_order_acquire);
        xr_amutex_unlock(&sem->lock);
        return available;
    }

    while (remaining > 0 && sem->wait_first) {
        XrCoroutine *coro = semaphore_waiter_pop_locked(sem);
        if (!coro)
            break;
        coro->sched_link = NULL;
        if (last) {
            last->sched_link = coro;
        } else {
            list = coro;
        }
        last = coro;
        remaining--;
    }
    if (remaining > 0)
        atomic_fetch_add_explicit(&sem->available, remaining, memory_order_release);
    xr_amutex_unlock(&sem->lock);

    int64_t failed_claims = semaphore_wake_list(sem, list, true);
    if (failed_claims > 0)
        (void) xr_semaphore_release(sem, failed_claims);
    return xr_semaphore_available(sem);
}

bool xr_semaphore_is_closed(XrSemaphore *sem) {
    return !sem || atomic_load_explicit(&sem->closed, memory_order_acquire);
}

int64_t xr_semaphore_available(XrSemaphore *sem) {
    return sem ? atomic_load_explicit(&sem->available, memory_order_acquire) : 0;
}

static void semaphore_finish_wait_if_current(XrCoroutine *coro, XrSemaphore *sem) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    void *current = atomic_load_explicit(&wait->semaphore_token.semaphore, memory_order_acquire);
    if (current == sem)
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
}

static bool semaphore_consume_resolved_wait(XrCoroutine *coro, XrSemaphore *sem, bool *result,
                                            XrSemaphoreWaitStatus *status) {
    if (!coro || !sem || !result || !status)
        return false;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return false;
    void *current = atomic_load_explicit(&wait->semaphore_token.semaphore, memory_order_acquire);
    if (current != sem)
        return false;

    int state = atomic_load_explicit(&wait->semaphore_token.state, memory_order_acquire);
    if (state == XR_SEMAPHORE_WAIT_RESOLVED) {
        bool ok = atomic_load_explicit(&wait->semaphore_token.success, memory_order_acquire);
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        *result = ok;
        *status = ok ? XR_SEMAPHORE_WAIT_ACQUIRED : XR_SEMAPHORE_WAIT_CLOSED;
        return true;
    }
    if (state == XR_SEMAPHORE_WAIT_CANCELLED) {
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        *result = false;
        *status = XR_SEMAPHORE_WAIT_CLOSED;
        return true;
    }
    return false;
}

XrSemaphoreWaitStatus xr_semaphore_acquire_for_coro(XrSemaphore *sem, XrCoroutine *coro,
                                                    bool *result) {
    if (!sem || !result)
        return XR_SEMAPHORE_WAIT_ERROR;
    XrSemaphoreWaitStatus replay_status = XR_SEMAPHORE_WAIT_ERROR;
    if (semaphore_consume_resolved_wait(coro, sem, result, &replay_status))
        return replay_status;
    if (xr_semaphore_try_acquire(sem)) {
        semaphore_finish_wait_if_current(coro, sem);
        *result = true;
        return XR_SEMAPHORE_WAIT_ACQUIRED;
    }
    if (xr_semaphore_is_closed(sem)) {
        semaphore_finish_wait_if_current(coro, sem);
        *result = false;
        return XR_SEMAPHORE_WAIT_CLOSED;
    }
    if (!coro)
        return XR_SEMAPHORE_WAIT_ERROR;
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return XR_SEMAPHORE_WAIT_ERROR;
    xr_semaphore_wait_token_prepare(&wait->semaphore_token, sem);

    xr_amutex_lock(&sem->lock);
    if (xr_semaphore_try_acquire(sem)) {
        xr_amutex_unlock(&sem->lock);
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        *result = true;
        return XR_SEMAPHORE_WAIT_ACQUIRED;
    }
    if (xr_semaphore_is_closed(sem)) {
        xr_amutex_unlock(&sem->lock);
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        *result = false;
        return XR_SEMAPHORE_WAIT_CLOSED;
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_SEMAPHORE >> XR_CORO_WAIT_SHIFT);
    XrWorker *worker_state = xr_current_worker();
    if (worker_state)
        atomic_store_explicit(&coro->affinity_p, worker_state->p.id, memory_order_relaxed);
    if (!semaphore_waiter_enqueue_locked(sem, coro)) {
        xr_amutex_unlock(&sem->lock);
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
        return XR_SEMAPHORE_WAIT_ERROR;
    }
    xr_semaphore_wait_token_commit(&wait->semaphore_token);
    (void) xr_coro_publish_wait_block(coro);
    xr_amutex_unlock(&sem->lock);
    return XR_SEMAPHORE_WAIT_BLOCKED;
}

XrSemaphoreWaitStatus xr_semaphore_acquire_resume_for_coro(XrCoroutine *coro, bool *result) {
    if (!coro || !result)
        return XR_SEMAPHORE_WAIT_ERROR;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return XR_SEMAPHORE_WAIT_ERROR;
    XrSemaphore *sem = (XrSemaphore *) atomic_load_explicit(&wait->semaphore_token.semaphore,
                                                            memory_order_acquire);
    int state = atomic_load_explicit(&wait->semaphore_token.state, memory_order_acquire);
    if (!sem)
        return XR_SEMAPHORE_WAIT_ERROR;
    if (state == XR_SEMAPHORE_WAIT_CANCELLED) {
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        *result = false;
        return XR_SEMAPHORE_WAIT_CLOSED;
    }
    if (state == XR_SEMAPHORE_WAIT_RESOLVED) {
        bool ok = atomic_load_explicit(&wait->semaphore_token.success, memory_order_acquire);
        xr_semaphore_wait_token_finish(&wait->semaphore_token);
        *result = ok;
        return ok ? XR_SEMAPHORE_WAIT_ACQUIRED : XR_SEMAPHORE_WAIT_CLOSED;
    }
    return xr_semaphore_acquire_for_coro(sem, coro, result);
}

void xr_semaphore_close(XrSemaphore *sem) {
    if (!sem)
        return;
    bool was_closed = atomic_exchange_explicit(&sem->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;

    XrCoroutine *list = NULL;
    XrCoroutine *last = NULL;
    xr_amutex_lock(&sem->lock);
    while (sem->wait_first) {
        XrCoroutine *coro = semaphore_waiter_pop_locked(sem);
        if (!coro)
            break;
        coro->sched_link = NULL;
        if (last) {
            last->sched_link = coro;
        } else {
            list = coro;
        }
        last = coro;
    }
    xr_amutex_unlock(&sem->lock);
    (void) semaphore_wake_list(sem, list, false);
}
