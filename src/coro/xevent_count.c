/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xevent_count.c - Shared epoch/event-count primitive
 */

#include "xevent_count.h"

#include <stddef.h>
#include <string.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xsystem_heap.h"
#include "xblock.h"
#include "xcoroutine.h"
#include "xevent_count_wait.h"
#include "xworker.h"
#include "xyieldable.h"

static XrRuntime *event_count_runtime(XrEventCount *event) {
    return event ? (XrRuntime *) event->scheduler : NULL;
}

static bool event_count_runtime_can_wake(XrRuntime *runtime) {
    return runtime && runtime->workers && runtime->worker_count > 0;
}

static bool event_count_waiter_enqueue_locked(XrEventCount *event, XrCoroutine *coro) {
    XR_DCHECK(event != NULL, "event_count_waiter_enqueue_locked: NULL event");
    XR_DCHECK(coro != NULL, "event_count_waiter_enqueue_locked: NULL coro");
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;

    ext->wait_link = NULL;
    ext->wait_prev = event->wait_last;
    if (event->wait_last) {
        event->wait_last->ext->wait_link = coro;
    } else {
        event->wait_first = coro;
    }
    event->wait_last = coro;
    atomic_fetch_add_explicit(&event->waiter_count, 1, memory_order_relaxed);
    return true;
}

static XrCoroutine *event_count_waiter_pop_locked(XrEventCount *event) {
    XR_DCHECK(event != NULL, "event_count_waiter_pop_locked: NULL event");
    XrCoroutine *coro = event->wait_first;
    if (!coro)
        return NULL;
    xr_event_count_waiter_unlink_locked(event, coro);
    return coro;
}

typedef struct XrEventCountWakeBatch {
    XrCoroutine *first;
    XrCoroutine *last;
    int count;
} XrEventCountWakeBatch;

enum {
    XR_EVENT_COUNT_STACK_WAKE_BATCHES = 64
};

static void event_count_wake_batch_append(XrEventCountWakeBatch *batch, XrCoroutine *coro) {
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

static bool event_count_claim_waiter(XrEventCount *event, XrCoroutine *coro, XrRuntime *runtime,
                                     int64_t epoch, int *target_id_out) {
    if (!event_count_runtime_can_wake(runtime) || !coro || !target_id_out)
        return false;
    if (!xr_coro_claim_wake(coro))
        return false;

    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (wait)
        xr_event_count_wait_token_resolve(&wait->event_count_token, epoch);
    xr_coro_resume_store(coro, XR_RESUME_OK);
    int target_id = xr_coro_wake_target_id(coro);
    if (target_id < 0 || target_id >= runtime->worker_count)
        target_id = 0;
    *target_id_out = target_id;
    (void) event;
    return true;
}

static void event_count_wake_list(XrEventCount *event, XrCoroutine *list, int64_t epoch) {
    if (!event || !list)
        return;
    XrRuntime *runtime = event_count_runtime(event);
    if (!event_count_runtime_can_wake(runtime))
        return;

    XrWorker *current = xr_current_worker();
    bool current_matches = current && current->p.runtime == runtime;
    XrEventCountWakeBatch local_batch = {0};
    XrEventCountWakeBatch stack_batches[XR_EVENT_COUNT_STACK_WAKE_BATCHES];
    XrEventCountWakeBatch *remote_batches = NULL;
    bool remote_batches_heap = false;
    if (runtime->worker_count > 0) {
        size_t batch_count = (size_t) runtime->worker_count;
        if (batch_count <= XR_EVENT_COUNT_STACK_WAKE_BATCHES) {
            memset(stack_batches, 0, batch_count * sizeof(XrEventCountWakeBatch));
            remote_batches = stack_batches;
        } else {
            remote_batches =
                (XrEventCountWakeBatch *) xr_calloc(batch_count, sizeof(XrEventCountWakeBatch));
            remote_batches_heap = remote_batches != NULL;
        }
    }
    int remote_ready_count = 0;

    while (list) {
        XrCoroutine *next = list->sched_link;
        list->sched_link = NULL;
        int target_id = 0;
        if (event_count_claim_waiter(event, list, runtime, epoch, &target_id)) {
            if (current_matches && target_id == current->p.id) {
                event_count_wake_batch_append(&local_batch, list);
            } else if (remote_batches) {
                event_count_wake_batch_append(&remote_batches[target_id], list);
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
            XrEventCountWakeBatch *batch = &remote_batches[i];
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

static int event_count_normalize_worker_hint(XrEventCount *event, int64_t worker_hint) {
    XrRuntime *runtime = event_count_runtime(event);
    if (!event_count_runtime_can_wake(runtime) || worker_hint < 0)
        return -1;
    return (int) ((uint64_t) worker_hint % (uint64_t) runtime->worker_count);
}

XrEventCount *xr_event_count_new(XrRuntimeCore *core, XrRuntime *scheduler, int64_t epoch) {
    if (!core || !core->sys_heap)
        return NULL;
    if (epoch < 0)
        epoch = 0;
    XrEventCount *event = (XrEventCount *) xr_sysheap_alloc_shared(
        core->sys_heap, sizeof(XrEventCount), XR_TEVENTCOUNT);
    if (!event)
        return NULL;
    xr_amutex_init(&event->lock);
    atomic_store_explicit(&event->epoch, epoch, memory_order_relaxed);
    atomic_store_explicit(&event->waiter_count, 0, memory_order_relaxed);
    atomic_store_explicit(&event->closed, false, memory_order_relaxed);
    event->wait_first = NULL;
    event->wait_last = NULL;
    event->core = core;
    event->scheduler = scheduler;
    return event;
}

int64_t xr_event_count_advance(XrEventCount *event, int64_t step) {
    if (!event)
        return -1;
    if (step <= 0)
        step = 1;

    XrCoroutine *list = NULL;
    XrCoroutine *last = NULL;
    int64_t next_epoch = -1;

    xr_amutex_lock(&event->lock);
    if (atomic_load_explicit(&event->closed, memory_order_acquire)) {
        next_epoch = -1;
    } else {
        int64_t old = atomic_load_explicit(&event->epoch, memory_order_acquire);
        if (old > INT64_MAX - step)
            step = INT64_MAX - old;
        next_epoch = old + step;
        atomic_store_explicit(&event->epoch, next_epoch, memory_order_release);
        while (event->wait_first) {
            XrCoroutine *coro = event_count_waiter_pop_locked(event);
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
    }
    xr_amutex_unlock(&event->lock);

    if (next_epoch >= 0)
        event_count_wake_list(event, list, next_epoch);
    return next_epoch;
}

bool xr_event_count_is_closed(XrEventCount *event) {
    return !event || atomic_load_explicit(&event->closed, memory_order_acquire);
}

int64_t xr_event_count_epoch(XrEventCount *event) {
    return event ? atomic_load_explicit(&event->epoch, memory_order_acquire) : -1;
}

static void event_count_finish_wait_if_current(XrCoroutine *coro, XrEventCount *event) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    void *current = atomic_load_explicit(&wait->event_count_token.event, memory_order_acquire);
    if (current == event)
        xr_event_count_wait_token_finish(&wait->event_count_token);
}

static bool event_count_consume_resolved_wait(XrCoroutine *coro, XrEventCount *event,
                                              int64_t *result_epoch,
                                              XrEventCountWaitStatus *status) {
    if (!coro || !event || !result_epoch || !status)
        return false;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return false;
    void *current = atomic_load_explicit(&wait->event_count_token.event, memory_order_acquire);
    if (current != event)
        return false;

    int state = atomic_load_explicit(&wait->event_count_token.state, memory_order_acquire);
    if (state == XR_EVENT_COUNT_WAIT_RESOLVED) {
        int64_t epoch =
            atomic_load_explicit(&wait->event_count_token.result_epoch, memory_order_acquire);
        xr_event_count_wait_token_finish(&wait->event_count_token);
        *result_epoch = epoch;
        *status = epoch >= 0 ? XR_EVENT_COUNT_WAIT_CHANGED : XR_EVENT_COUNT_WAIT_CLOSED;
        return true;
    }
    if (state == XR_EVENT_COUNT_WAIT_CANCELLED) {
        xr_event_count_wait_token_finish(&wait->event_count_token);
        *result_epoch = -1;
        *status = XR_EVENT_COUNT_WAIT_CLOSED;
        return true;
    }
    return false;
}

XrEventCountWaitStatus xr_event_count_wait_for_coro(XrEventCount *event, XrCoroutine *coro,
                                                    int64_t last_epoch, int64_t worker_hint,
                                                    int64_t *result_epoch) {
    if (!event || !result_epoch)
        return XR_EVENT_COUNT_WAIT_ERROR;
    XrEventCountWaitStatus replay_status = XR_EVENT_COUNT_WAIT_ERROR;
    if (event_count_consume_resolved_wait(coro, event, result_epoch, &replay_status))
        return replay_status;
    if (xr_event_count_is_closed(event)) {
        event_count_finish_wait_if_current(coro, event);
        *result_epoch = -1;
        return XR_EVENT_COUNT_WAIT_CLOSED;
    }
    int64_t current = xr_event_count_epoch(event);
    if (current != last_epoch) {
        event_count_finish_wait_if_current(coro, event);
        *result_epoch = current;
        return XR_EVENT_COUNT_WAIT_CHANGED;
    }
    if (!coro)
        return XR_EVENT_COUNT_WAIT_ERROR;
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return XR_EVENT_COUNT_WAIT_ERROR;
    xr_event_count_wait_token_prepare(&wait->event_count_token, event, last_epoch);

    xr_amutex_lock(&event->lock);
    if (xr_event_count_is_closed(event)) {
        xr_amutex_unlock(&event->lock);
        xr_event_count_wait_token_finish(&wait->event_count_token);
        *result_epoch = -1;
        return XR_EVENT_COUNT_WAIT_CLOSED;
    }
    current = xr_event_count_epoch(event);
    if (current != last_epoch) {
        xr_amutex_unlock(&event->lock);
        xr_event_count_wait_token_finish(&wait->event_count_token);
        *result_epoch = current;
        return XR_EVENT_COUNT_WAIT_CHANGED;
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_EVENT_COUNT >> XR_CORO_WAIT_SHIFT);
    int hinted_worker = event_count_normalize_worker_hint(event, worker_hint);
    if (hinted_worker >= 0) {
        atomic_store_explicit(&coro->affinity_p, hinted_worker, memory_order_relaxed);
    } else {
        XrWorker *worker_state = xr_current_worker();
        if (worker_state)
            atomic_store_explicit(&coro->affinity_p, worker_state->p.id, memory_order_relaxed);
    }
    if (!event_count_waiter_enqueue_locked(event, coro)) {
        xr_amutex_unlock(&event->lock);
        xr_event_count_wait_token_finish(&wait->event_count_token);
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
        return XR_EVENT_COUNT_WAIT_ERROR;
    }
    xr_event_count_wait_token_commit(&wait->event_count_token);
    (void) xr_coro_publish_wait_block(coro);
    xr_amutex_unlock(&event->lock);
    return XR_EVENT_COUNT_WAIT_BLOCKED;
}

XrEventCountWaitStatus xr_event_count_wait_resume_for_coro(XrCoroutine *coro,
                                                           int64_t *result_epoch) {
    if (!coro || !result_epoch)
        return XR_EVENT_COUNT_WAIT_ERROR;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return XR_EVENT_COUNT_WAIT_ERROR;
    XrEventCount *event =
        (XrEventCount *) atomic_load_explicit(&wait->event_count_token.event, memory_order_acquire);
    int state = atomic_load_explicit(&wait->event_count_token.state, memory_order_acquire);
    int64_t last_epoch =
        atomic_load_explicit(&wait->event_count_token.expected_epoch, memory_order_acquire);
    if (!event)
        return XR_EVENT_COUNT_WAIT_ERROR;
    if (state == XR_EVENT_COUNT_WAIT_CANCELLED) {
        xr_event_count_wait_token_finish(&wait->event_count_token);
        *result_epoch = -1;
        return XR_EVENT_COUNT_WAIT_CLOSED;
    }
    if (state == XR_EVENT_COUNT_WAIT_RESOLVED) {
        int64_t epoch =
            atomic_load_explicit(&wait->event_count_token.result_epoch, memory_order_acquire);
        xr_event_count_wait_token_finish(&wait->event_count_token);
        *result_epoch = epoch;
        return epoch >= 0 ? XR_EVENT_COUNT_WAIT_CHANGED : XR_EVENT_COUNT_WAIT_CLOSED;
    }
    return xr_event_count_wait_for_coro(event, coro, last_epoch, -1, result_epoch);
}

void xr_event_count_close(XrEventCount *event) {
    if (!event)
        return;
    bool was_closed = atomic_exchange_explicit(&event->closed, true, memory_order_acq_rel);
    if (was_closed)
        return;

    XrCoroutine *list = NULL;
    XrCoroutine *last = NULL;
    xr_amutex_lock(&event->lock);
    while (event->wait_first) {
        XrCoroutine *coro = event_count_waiter_pop_locked(event);
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
    xr_amutex_unlock(&event->lock);
    event_count_wake_list(event, list, -1);
}
