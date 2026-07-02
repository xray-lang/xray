/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcountdown_latch.c - Shared countdown barrier primitive
 */

#include "xcountdown_latch.h"

#include <stddef.h>
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/xshared.h"
#include "xblock.h"
#include "xcoroutine.h"
#include "xcountdown_latch_wait.h"
#include "xworker.h"
#include "xyieldable.h"

static XrRuntime *countdown_latch_runtime(XrCountdownLatch *latch) {
    return latch ? (XrRuntime *) latch->scheduler : NULL;
}

static bool countdown_latch_runtime_can_wake(XrRuntime *runtime) {
    return runtime && runtime->workers && runtime->worker_count > 0;
}

static bool countdown_latch_waiter_enqueue_locked(XrCountdownLatch *latch, XrCoroutine *coro) {
    XR_DCHECK(latch != NULL, "countdown_latch_waiter_enqueue_locked: NULL latch");
    XR_DCHECK(coro != NULL, "countdown_latch_waiter_enqueue_locked: NULL coro");
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;

    ext->wait_link = NULL;
    ext->wait_prev = latch->wait_last;
    if (latch->wait_last) {
        latch->wait_last->ext->wait_link = coro;
    } else {
        latch->wait_first = coro;
    }
    latch->wait_last = coro;
    atomic_fetch_add_explicit(&latch->waiter_count, 1, memory_order_relaxed);
    return true;
}

static XrCoroutine *countdown_latch_waiter_pop_locked(XrCountdownLatch *latch) {
    XR_DCHECK(latch != NULL, "countdown_latch_waiter_pop_locked: NULL latch");
    XrCoroutine *coro = latch->wait_first;
    if (!coro)
        return NULL;
    xr_countdown_latch_waiter_unlink_locked(latch, coro);
    return coro;
}

typedef struct XrCountdownLatchWakeBatch {
    XrCoroutine *first;
    XrCoroutine *last;
    int count;
} XrCountdownLatchWakeBatch;

enum {
    XR_COUNTDOWN_LATCH_STACK_WAKE_BATCHES = 64
};

static void countdown_latch_wake_batch_append(XrCountdownLatchWakeBatch *batch, XrCoroutine *coro) {
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

static bool countdown_latch_claim_waiter(XrCountdownLatch *latch, XrCoroutine *coro,
                                         XrRuntime *runtime, bool success, int *target_id_out) {
    if (!countdown_latch_runtime_can_wake(runtime) || !coro || !target_id_out)
        return false;
    if (!xr_coro_claim_wake(coro))
        return false;

    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (wait)
        xr_countdown_latch_wait_token_resolve(&wait->countdown_latch_token, success);
    xr_coro_resume_store(coro, XR_RESUME_OK);
    int target_id = xr_coro_wake_target_id(coro);
    if (target_id < 0 || target_id >= runtime->worker_count)
        target_id = 0;
    *target_id_out = target_id;
    (void) latch;
    return true;
}

static void countdown_latch_wake_all(XrCountdownLatch *latch, bool success) {
    if (!latch)
        return;
    XrRuntime *runtime = countdown_latch_runtime(latch);
    if (!countdown_latch_runtime_can_wake(runtime))
        return;

    XrCoroutine *list = NULL;
    XrCoroutine *last = NULL;

    xr_amutex_lock(&latch->lock);
    while (latch->wait_first) {
        XrCoroutine *coro = countdown_latch_waiter_pop_locked(latch);
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
    xr_amutex_unlock(&latch->lock);

    XrWorker *current = xr_current_worker();
    bool current_matches = current && current->p.runtime == runtime;
    XrCountdownLatchWakeBatch local_batch = {0};
    XrCountdownLatchWakeBatch stack_batches[XR_COUNTDOWN_LATCH_STACK_WAKE_BATCHES];
    XrCountdownLatchWakeBatch *remote_batches = NULL;
    bool remote_batches_heap = false;
    if (runtime && runtime->worker_count > 0) {
        size_t batch_count = (size_t) runtime->worker_count;
        if (batch_count <= XR_COUNTDOWN_LATCH_STACK_WAKE_BATCHES) {
            memset(stack_batches, 0, batch_count * sizeof(XrCountdownLatchWakeBatch));
            remote_batches = stack_batches;
        } else {
            remote_batches = (XrCountdownLatchWakeBatch *) xr_calloc(
                batch_count, sizeof(XrCountdownLatchWakeBatch));
            remote_batches_heap = remote_batches != NULL;
        }
    }
    int remote_ready_count = 0;

    while (list) {
        XrCoroutine *next = list->sched_link;
        list->sched_link = NULL;
        int target_id = 0;
        if (countdown_latch_claim_waiter(latch, list, runtime, success, &target_id)) {
            if (current_matches && target_id == current->p.id) {
                countdown_latch_wake_batch_append(&local_batch, list);
            } else if (remote_batches) {
                countdown_latch_wake_batch_append(&remote_batches[target_id], list);
                remote_ready_count++;
            } else {
                xr_worker_inbox_enqueue(runtime, target_id, list);
                remote_ready_count++;
            }
        }
        list = next;
    }

    if (local_batch.first) {
        (void) xr_worker_push_lifo_batch(current, local_batch.first);
        xr_runtime_wake_idle_worker(runtime);
    }
    if (remote_batches) {
        for (int i = 0; i < runtime->worker_count; i++) {
            XrCountdownLatchWakeBatch *batch = &remote_batches[i];
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
}

XrCountdownLatch *xr_countdown_latch_new(XrRuntimeCore *core, XrRuntime *scheduler, int64_t count) {
    if (!core || !core->sys_heap)
        return NULL;
    if (count < 0)
        count = 0;
    XrCountdownLatch *latch = (XrCountdownLatch *) xr_sysheap_alloc_shared(
        core->sys_heap, sizeof(XrCountdownLatch), XR_TCOUNTDOWNLATCH);
    if (!latch)
        return NULL;
    xr_amutex_init(&latch->lock);
    atomic_store_explicit(&latch->remaining, count, memory_order_relaxed);
    atomic_store_explicit(&latch->waiter_count, 0, memory_order_relaxed);
    atomic_store_explicit(&latch->closed, false, memory_order_relaxed);
    latch->wait_first = NULL;
    latch->wait_last = NULL;
    latch->core = core;
    latch->scheduler = scheduler;
    return latch;
}

bool xr_countdown_latch_reset(XrCountdownLatch *latch, int64_t count) {
    if (!latch || count < 0 || atomic_load_explicit(&latch->closed, memory_order_acquire))
        return false;
    int64_t expected = 0;
    if (atomic_load_explicit(&latch->waiter_count, memory_order_acquire) == 0 &&
        atomic_compare_exchange_strong_explicit(&latch->remaining, &expected, count,
                                                memory_order_release, memory_order_acquire)) {
        return true;
    }
    bool ok = false;
    xr_amutex_lock(&latch->lock);
    if (atomic_load_explicit(&latch->remaining, memory_order_acquire) == 0 &&
        atomic_load_explicit(&latch->waiter_count, memory_order_acquire) == 0) {
        atomic_store_explicit(&latch->remaining, count, memory_order_release);
        ok = true;
    }
    xr_amutex_unlock(&latch->lock);
    return ok;
}

int64_t xr_countdown_latch_done(XrCountdownLatch *latch, int64_t count) {
    if (!latch)
        return 0;
    if (count <= 0)
        count = 1;
    int64_t old = atomic_load_explicit(&latch->remaining, memory_order_acquire);
    int64_t next = old;
    while (old > 0) {
        next = old > count ? old - count : 0;
        if (atomic_compare_exchange_weak_explicit(&latch->remaining, &old, next,
                                                  memory_order_acq_rel, memory_order_acquire)) {
            if (next == 0)
                countdown_latch_wake_all(latch, true);
            return next;
        }
    }
    return old;
}

bool xr_countdown_latch_try_wait(XrCountdownLatch *latch) {
    return latch && atomic_load_explicit(&latch->remaining, memory_order_acquire) == 0 &&
           !atomic_load_explicit(&latch->closed, memory_order_acquire);
}

bool xr_countdown_latch_is_closed(XrCountdownLatch *latch) {
    return !latch || atomic_load_explicit(&latch->closed, memory_order_acquire);
}

int64_t xr_countdown_latch_remaining(XrCountdownLatch *latch) {
    return latch ? atomic_load_explicit(&latch->remaining, memory_order_acquire) : 0;
}

static void countdown_latch_finish_wait_if_current(XrCoroutine *coro, XrCountdownLatch *latch) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    void *current = atomic_load_explicit(&wait->countdown_latch_token.latch, memory_order_acquire);
    if (current == latch)
        xr_countdown_latch_wait_token_finish(&wait->countdown_latch_token);
}

XrCountdownLatchWaitStatus xr_countdown_latch_wait_for_coro(XrCountdownLatch *latch,
                                                            XrCoroutine *coro, bool *result) {
    if (!latch || !result)
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    if (xr_countdown_latch_try_wait(latch)) {
        countdown_latch_finish_wait_if_current(coro, latch);
        *result = true;
        return XR_COUNTDOWN_LATCH_WAIT_DONE;
    }
    if (xr_countdown_latch_is_closed(latch)) {
        countdown_latch_finish_wait_if_current(coro, latch);
        *result = false;
        return XR_COUNTDOWN_LATCH_WAIT_CLOSED;
    }
    if (!coro)
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    xr_countdown_latch_wait_token_prepare(&wait->countdown_latch_token, latch);

    xr_amutex_lock(&latch->lock);
    if (xr_countdown_latch_try_wait(latch)) {
        xr_amutex_unlock(&latch->lock);
        xr_countdown_latch_wait_token_finish(&wait->countdown_latch_token);
        *result = true;
        return XR_COUNTDOWN_LATCH_WAIT_DONE;
    }
    if (xr_countdown_latch_is_closed(latch)) {
        xr_amutex_unlock(&latch->lock);
        xr_countdown_latch_wait_token_finish(&wait->countdown_latch_token);
        *result = false;
        return XR_COUNTDOWN_LATCH_WAIT_CLOSED;
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_COUNTDOWN_LATCH >> XR_CORO_WAIT_SHIFT);
    XrWorker *worker_state = xr_current_worker();
    if (worker_state)
        atomic_store_explicit(&coro->affinity_p, worker_state->p.id, memory_order_relaxed);
    if (!countdown_latch_waiter_enqueue_locked(latch, coro)) {
        xr_amutex_unlock(&latch->lock);
        xr_countdown_latch_wait_token_finish(&wait->countdown_latch_token);
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    }
    xr_countdown_latch_wait_token_commit(&wait->countdown_latch_token);
    (void) xr_coro_publish_wait_block(coro);
    xr_amutex_unlock(&latch->lock);
    return XR_COUNTDOWN_LATCH_WAIT_BLOCKED;
}

XrCountdownLatchWaitStatus xr_countdown_latch_wait_resume_for_coro(XrCoroutine *coro,
                                                                   bool *result) {
    if (!coro || !result)
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    XrCountdownLatch *latch = (XrCountdownLatch *) atomic_load_explicit(
        &wait->countdown_latch_token.latch, memory_order_acquire);
    int state = atomic_load_explicit(&wait->countdown_latch_token.state, memory_order_acquire);
    if (!latch)
        return XR_COUNTDOWN_LATCH_WAIT_ERROR;
    if (state == XR_COUNTDOWN_LATCH_WAIT_CANCELLED) {
        xr_countdown_latch_wait_token_finish(&wait->countdown_latch_token);
        *result = false;
        return XR_COUNTDOWN_LATCH_WAIT_CLOSED;
    }
    if (state == XR_COUNTDOWN_LATCH_WAIT_RESOLVED) {
        bool ok = atomic_load_explicit(&wait->countdown_latch_token.success, memory_order_acquire);
        xr_countdown_latch_wait_token_finish(&wait->countdown_latch_token);
        *result = ok;
        return ok ? XR_COUNTDOWN_LATCH_WAIT_DONE : XR_COUNTDOWN_LATCH_WAIT_CLOSED;
    }
    return xr_countdown_latch_wait_for_coro(latch, coro, result);
}

void xr_countdown_latch_close(XrCountdownLatch *latch) {
    if (!latch)
        return;
    bool was_closed = atomic_exchange_explicit(&latch->closed, true, memory_order_acq_rel);
    if (!was_closed)
        countdown_latch_wake_all(latch, false);
}
