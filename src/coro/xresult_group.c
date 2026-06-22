/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresult_group.c - Shared scalar result reducer primitive
 */

#include "xresult_group.h"

#include <stddef.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/xshared.h"
#include "xblock.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "xresult_group_wait.h"
#include "xyieldable.h"

static XrRuntime *result_group_runtime(XrResultGroup *g) {
    return g ? (XrRuntime *) g->scheduler : NULL;
}

static bool result_group_runtime_can_wake(XrRuntime *runtime) {
    return runtime && runtime->workers && runtime->worker_count > 0;
}

static XrRuntime *result_group_stats_runtime(XrResultGroup *g) {
    XrRuntime *runtime = result_group_runtime(g);
    return xr_sched_stats_enabled(runtime) ? runtime : NULL;
}

#define RESULT_GROUP_METRIC_INC(g, field)                                                          \
    do {                                                                                           \
        XrRuntime *_rt = result_group_stats_runtime((g));                                          \
        if (_rt)                                                                                   \
            xr_sched_metric_inc(_rt, &_rt->sched_stats.field);                                     \
    } while (0)

#define RESULT_GROUP_METRIC_ADD(g, field, value)                                                   \
    do {                                                                                           \
        XrRuntime *_rt = result_group_stats_runtime((g));                                          \
        if (_rt)                                                                                   \
            xr_sched_metric_add(_rt, &_rt->sched_stats.field, (uint64_t) (value));                 \
    } while (0)

static XrResultGroupBatch *result_group_batch_new(int64_t value, uint32_t count) {
    XrResultGroupBatch *batch = (XrResultGroupBatch *) xr_malloc(sizeof(XrResultGroupBatch));
    if (!batch)
        return NULL;
    batch->value = value;
    batch->count = count;
    batch->next = NULL;
    return batch;
}

static void result_group_enqueue_batch_locked(XrResultGroup *g, XrResultGroupBatch *batch) {
    XR_DCHECK(g != NULL, "result_group_enqueue_batch_locked: NULL group");
    XR_DCHECK(batch != NULL, "result_group_enqueue_batch_locked: NULL batch");
    batch->next = NULL;
    if (g->batch_last) {
        g->batch_last->next = batch;
    } else {
        g->batch_first = batch;
    }
    g->batch_last = batch;
    atomic_fetch_add_explicit(&g->length, 1, memory_order_release);
    RESULT_GROUP_METRIC_INC(g, result_group_flush_count);
    RESULT_GROUP_METRIC_ADD(g, result_group_flush_item_count, batch->count);
}

static bool result_group_flush_locked(XrResultGroup *g) {
    XR_DCHECK(g != NULL, "result_group_flush_locked: NULL group");
    if (g->current_count == 0)
        return false;
    XrResultGroupBatch *batch = result_group_batch_new(g->current_value, g->current_count);
    if (!batch)
        return false;
    result_group_enqueue_batch_locked(g, batch);
    g->current_value = 0;
    g->current_count = 0;
    return true;
}

static bool result_group_pop_batch_locked(XrResultGroup *g, int64_t *out) {
    XR_DCHECK(g != NULL, "result_group_pop_batch_locked: NULL group");
    XR_DCHECK(out != NULL, "result_group_pop_batch_locked: NULL out");
    XrResultGroupBatch *batch = g->batch_first;
    if (!batch) {
        // Closed-drain fallback: close() flushes the accumulator into a
        // batch, but that flush allocates and can fail under OOM. Draining
        // the accumulator directly needs no allocation, so data added
        // before close() can never be silently lost. Gated on `closed` to
        // keep the batch-granularity contract for live groups.
        if (atomic_load_explicit(&g->closed, memory_order_relaxed) && g->current_count > 0) {
            *out = g->current_value;
            atomic_fetch_sub_explicit(&g->pending_count, g->current_count, memory_order_release);
            g->current_value = 0;
            g->current_count = 0;
            RESULT_GROUP_METRIC_INC(g, result_group_recv_count);
            return true;
        }
        return false;
    }
    g->batch_first = batch->next;
    if (!g->batch_first)
        g->batch_last = NULL;
    atomic_fetch_sub_explicit(&g->length, 1, memory_order_release);
    atomic_fetch_sub_explicit(&g->pending_count, batch->count, memory_order_release);
    *out = batch->value;
    xr_free(batch);
    RESULT_GROUP_METRIC_INC(g, result_group_recv_count);
    return true;
}

static bool result_group_waiter_enqueue_locked(XrResultGroup *g, XrCoroutine *coro) {
    XR_DCHECK(g != NULL, "result_group_waiter_enqueue_locked: NULL group");
    XR_DCHECK(coro != NULL, "result_group_waiter_enqueue_locked: NULL coro");
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;

    ext->wait_link = NULL;
    ext->wait_prev = g->wait_last;
    if (g->wait_last) {
        g->wait_last->ext->wait_link = coro;
    } else {
        g->wait_first = coro;
    }
    g->wait_last = coro;
    atomic_fetch_add_explicit(&g->waiter_count, 1, memory_order_relaxed);
    RESULT_GROUP_METRIC_INC(g, result_group_block_count);
    return true;
}

static XrCoroutine *result_group_waiter_pop_locked(XrResultGroup *g) {
    XR_DCHECK(g != NULL, "result_group_waiter_pop_locked: NULL group");
    XrCoroutine *coro = g->wait_first;
    if (!coro)
        return NULL;
    xr_result_group_waiter_unlink_locked(g, coro);
    return coro;
}

typedef struct XrResultGroupWakeBatch {
    XrCoroutine *first;
    XrCoroutine *last;
    int count;
} XrResultGroupWakeBatch;

static void result_group_wake_batch_append(XrResultGroupWakeBatch *batch, XrCoroutine *coro) {
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

static bool result_group_claim_waiter(XrResultGroup *g, XrCoroutine *coro, XrRuntime *runtime,
                                      int *target_id_out) {
    if (!result_group_runtime_can_wake(runtime) || !coro || !target_id_out)
        return false;
    if (!xr_coro_claim_wake(coro))
        return false;

    RESULT_GROUP_METRIC_INC(g, result_group_wake_count);
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (wait)
        xr_result_group_wait_token_resolve(&wait->result_group_token);
    xr_coro_resume_store(coro, XR_RESUME_OK);
    int target_id = xr_coro_wake_target_id(coro);
    if (target_id < 0 || target_id >= runtime->worker_count)
        target_id = 0;
    *target_id_out = target_id;
    return true;
}

static bool result_group_ready_waiter(XrResultGroup *g, XrCoroutine *coro) {
    XrRuntime *runtime = result_group_runtime(g);
    int target_id = 0;
    if (!result_group_claim_waiter(g, coro, runtime, &target_id))
        return false;

    XrWorker *current = xr_current_worker();
    if (current && current->p.runtime == runtime && target_id == current->p.id) {
        xr_worker_push_lifo(current, coro);
        xr_runtime_wake_idle_worker(runtime);
        return true;
    }

    xr_worker_inbox_enqueue(runtime, target_id, coro);
    if (atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed) == 0)
        xr_runtime_wake_idle_worker(runtime);
    return true;
}

static void result_group_wake_one(XrResultGroup *g) {
    if (!g)
        return;
    if (!result_group_runtime_can_wake(result_group_runtime(g)))
        return;
    for (;;) {
        xr_amutex_lock(&g->lock);
        XrCoroutine *coro = result_group_waiter_pop_locked(g);
        xr_amutex_unlock(&g->lock);
        if (!coro)
            return;
        if (result_group_ready_waiter(g, coro))
            return;
    }
}

static void result_group_wake_all(XrResultGroup *g) {
    if (!g)
        return;
    XrRuntime *runtime = result_group_runtime(g);
    if (!result_group_runtime_can_wake(runtime))
        return;

    xr_amutex_lock(&g->lock);
    XrCoroutine *list = g->wait_first;
    g->wait_first = NULL;
    g->wait_last = NULL;
    atomic_store_explicit(&g->waiter_count, 0, memory_order_relaxed);
    xr_amutex_unlock(&g->lock);

    XrWorker *current = xr_current_worker();
    bool current_matches = current && current->p.runtime == runtime;
    XrResultGroupWakeBatch local_batch = {0};
    XrResultGroupWakeBatch *remote_batches = NULL;
    if (runtime && runtime->worker_count > 0) {
        remote_batches = (XrResultGroupWakeBatch *) xr_calloc((size_t) runtime->worker_count,
                                                              sizeof(XrResultGroupWakeBatch));
    }
    int remote_ready_count = 0;

    while (list) {
        XrCoroutine *next = list->ext ? list->ext->wait_link : NULL;
        if (list->ext) {
            list->ext->wait_link = NULL;
            list->ext->wait_prev = NULL;
        }
        int target_id = 0;
        if (result_group_claim_waiter(g, list, runtime, &target_id)) {
            RESULT_GROUP_METRIC_INC(g, result_group_close_wake_count);
            if (current_matches && target_id == current->p.id) {
                result_group_wake_batch_append(&local_batch, list);
            } else if (remote_batches) {
                result_group_wake_batch_append(&remote_batches[target_id], list);
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
            XrResultGroupWakeBatch *batch = &remote_batches[i];
            if (batch->first) {
                xr_worker_inbox_enqueue_batch(runtime, i, batch->first, batch->last, batch->count);
            }
        }
        xr_free(remote_batches);
    }
    if (remote_ready_count > 0 &&
        atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed) == 0) {
        xr_runtime_wake_idle_worker(runtime);
    }
}

XrResultGroup *xr_result_group_new(XrRuntimeCore *core, XrRuntime *scheduler, uint32_t batch_size) {
    if (!core || !core->sys_heap)
        return NULL;
    if (batch_size == 0)
        batch_size = XR_RESULT_GROUP_DEFAULT_BATCH;
    if (batch_size > XR_RESULT_GROUP_MAX_BATCH)
        batch_size = XR_RESULT_GROUP_MAX_BATCH;

    XrResultGroup *g = (XrResultGroup *) xr_sysheap_alloc_shared(
        core->sys_heap, sizeof(XrResultGroup), XR_TRESULTGROUP);
    if (!g)
        return NULL;

    /* Atomic shared-RC like `shared const`: the compiler tracks the handle and
     * the last drop frees. NOT XR_OBJ_MANAGED (that no-ops drop -> leak). */
    xr_shared_set_refc(&g->hdr, 1);
    xr_amutex_init(&g->lock);
    atomic_store_explicit(&g->length, 0, memory_order_relaxed);
    atomic_store_explicit(&g->pending_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g->waiter_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g->closed, false, memory_order_relaxed);
    g->batch_size = batch_size;
    g->current_value = 0;
    g->current_count = 0;
    g->batch_first = NULL;
    g->batch_last = NULL;
    g->wait_first = NULL;
    g->wait_last = NULL;
    g->core = core;
    g->scheduler = scheduler;
    return g;
}

bool xr_result_group_add(XrResultGroup *g, int64_t value) {
    XR_DCHECK(g != NULL, "xr_result_group_add: NULL group");
    bool enqueued = false;
    bool ok = false;
    xr_amutex_lock(&g->lock);
    if (!atomic_load_explicit(&g->closed, memory_order_relaxed)) {
        XrResultGroupBatch *batch = NULL;
        if (g->current_count + 1u >= g->batch_size) {
            batch = result_group_batch_new(g->current_value + value, g->current_count + 1u);
        }
        if (g->current_count + 1u < g->batch_size || batch) {
            if (batch) {
                result_group_enqueue_batch_locked(g, batch);
                g->current_value = 0;
                g->current_count = 0;
                enqueued = true;
            } else {
                g->current_value += value;
                g->current_count++;
            }
            atomic_fetch_add_explicit(&g->pending_count, 1, memory_order_release);
            ok = true;
        }
    }
    xr_amutex_unlock(&g->lock);
    if (ok)
        RESULT_GROUP_METRIC_INC(g, result_group_add_count);
    if (enqueued)
        result_group_wake_one(g);
    return ok;
}

bool xr_result_group_flush(XrResultGroup *g) {
    if (!g)
        return false;
    bool enqueued = false;
    xr_amutex_lock(&g->lock);
    enqueued = result_group_flush_locked(g);
    xr_amutex_unlock(&g->lock);
    if (enqueued)
        result_group_wake_one(g);
    return enqueued;
}

bool xr_result_group_try_recv(XrResultGroup *g, int64_t *out) {
    if (!g || !out)
        return false;
    bool ok = false;
    xr_amutex_lock(&g->lock);
    ok = result_group_pop_batch_locked(g, out);
    xr_amutex_unlock(&g->lock);
    if (!ok)
        RESULT_GROUP_METRIC_INC(g, result_group_recv_empty_count);
    return ok;
}

void xr_result_group_close(XrResultGroup *g) {
    if (!g)
        return;
    RESULT_GROUP_METRIC_INC(g, result_group_close_count);
    xr_amutex_lock(&g->lock);
    atomic_store_explicit(&g->closed, true, memory_order_release);
    // Flush failure (OOM) is safe to ignore: once closed, the pop path
    // drains the accumulator directly without allocating.
    (void) result_group_flush_locked(g);
    xr_amutex_unlock(&g->lock);
    result_group_wake_all(g);
}

bool xr_result_group_is_closed(XrResultGroup *g) {
    return g ? atomic_load_explicit(&g->closed, memory_order_acquire) : true;
}

uint64_t xr_result_group_length(XrResultGroup *g) {
    return g ? atomic_load_explicit(&g->length, memory_order_acquire) : 0;
}

uint64_t xr_result_group_pending_count(XrResultGroup *g) {
    return g ? atomic_load_explicit(&g->pending_count, memory_order_acquire) : 0;
}

static void result_group_finish_wait_if_current(XrCoroutine *coro, XrResultGroup *g) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    void *current = atomic_load_explicit(&wait->result_group_token.group, memory_order_acquire);
    if (current == g)
        xr_result_group_wait_token_finish(&wait->result_group_token);
}

XrResultGroupRecvStatus xr_result_group_recv_for_coro(XrResultGroup *g, XrCoroutine *coro,
                                                      XrValue *result) {
    if (!g || !result)
        return XR_RESULT_GROUP_RECV_ERROR;
    int64_t value = 0;
    if (xr_result_group_try_recv(g, &value)) {
        result_group_finish_wait_if_current(coro, g);
        *result = xr_int(value);
        return XR_RESULT_GROUP_RECV_DONE;
    }
    if (xr_result_group_is_closed(g)) {
        result_group_finish_wait_if_current(coro, g);
        *result = xr_null();
        return XR_RESULT_GROUP_RECV_DONE;
    }
    if (!coro)
        return XR_RESULT_GROUP_RECV_ERROR;
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return XR_RESULT_GROUP_RECV_ERROR;
    xr_result_group_wait_token_prepare(&wait->result_group_token, g);

    xr_amutex_lock(&g->lock);
    if (result_group_pop_batch_locked(g, &value)) {
        xr_amutex_unlock(&g->lock);
        xr_result_group_wait_token_finish(&wait->result_group_token);
        *result = xr_int(value);
        return XR_RESULT_GROUP_RECV_DONE;
    }
    if (xr_result_group_is_closed(g)) {
        xr_amutex_unlock(&g->lock);
        xr_result_group_wait_token_finish(&wait->result_group_token);
        *result = xr_null();
        return XR_RESULT_GROUP_RECV_DONE;
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_RESULTGROUP >> XR_CORO_WAIT_SHIFT);
    XrWorker *worker_state = xr_current_worker();
    if (worker_state)
        atomic_store_explicit(&coro->affinity_p, worker_state->p.id, memory_order_relaxed);
    if (!result_group_waiter_enqueue_locked(g, coro)) {
        xr_amutex_unlock(&g->lock);
        xr_result_group_wait_token_finish(&wait->result_group_token);
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
        return XR_RESULT_GROUP_RECV_ERROR;
    }
    xr_result_group_wait_token_commit(&wait->result_group_token);
    (void) xr_coro_publish_wait_block(coro);
    xr_amutex_unlock(&g->lock);
    return XR_RESULT_GROUP_RECV_BLOCKED;
}

XrResultGroupRecvStatus xr_result_group_recv_resume_for_coro(XrCoroutine *coro, XrValue *result) {
    if (!coro || !result)
        return XR_RESULT_GROUP_RECV_ERROR;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return XR_RESULT_GROUP_RECV_ERROR;
    XrResultGroup *g = (XrResultGroup *) atomic_load_explicit(&wait->result_group_token.group,
                                                              memory_order_acquire);
    if (!g)
        return XR_RESULT_GROUP_RECV_ERROR;
    return xr_result_group_recv_for_coro(g, coro, result);
}

#undef RESULT_GROUP_METRIC_ADD
#undef RESULT_GROUP_METRIC_INC
