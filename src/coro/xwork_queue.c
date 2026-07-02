/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwork_queue.c - Shared sharded work queue primitive
 */

#include "xwork_queue.h"

#include <stddef.h>

#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/mem/xsystem_heap.h"
#include "../runtime/xshared.h"
#include "xblock.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"
#include "xworker.h"
#include "xwork_queue_wait.h"
#include "xyieldable.h"

static bool shard_init(XrWorkQueueShard *shard, uint32_t capacity) {
    XR_DCHECK(shard != NULL, "shard_init: NULL shard");
    xr_amutex_init(&shard->lock);
    shard->items = NULL;
    shard->capacity = 0;
    shard->head = 0;
    shard->count = 0;

    if (capacity == 0)
        return true;
    shard->items = (XrValue *) xr_calloc(capacity, sizeof(XrValue));
    if (!shard->items)
        return false;
    shard->capacity = capacity;
    return true;
}

static void shard_reset(XrWorkQueueShard *shard) {
    if (!shard)
        return;
    xr_free(shard->items);
    shard->items = NULL;
    shard->capacity = 0;
    shard->head = 0;
    shard->count = 0;
}

static bool shard_grow(XrWorkQueueShard *shard) {
    XR_DCHECK(shard != NULL, "shard_grow: NULL shard");
    uint32_t old_cap = shard->capacity;
    uint32_t new_cap =
        old_cap < XR_WORK_QUEUE_DEFAULT_CAPACITY ? XR_WORK_QUEUE_DEFAULT_CAPACITY : old_cap * 2u;
    if (new_cap <= old_cap || new_cap > XR_WORK_QUEUE_MAX_CAPACITY)
        return false;

    XrValue *items = (XrValue *) xr_calloc(new_cap, sizeof(XrValue));
    if (!items)
        return false;

    for (uint32_t i = 0; i < shard->count; i++) {
        uint32_t idx = old_cap == 0 ? 0 : (shard->head + i) % old_cap;
        items[i] = old_cap == 0 ? xr_null() : shard->items[idx];
    }
    xr_free(shard->items);
    shard->items = items;
    shard->capacity = new_cap;
    shard->head = 0;
    return true;
}

static bool shard_push_tail(XrWorkQueueShard *shard, XrValue value) {
    XR_DCHECK(shard != NULL, "shard_push_tail: NULL shard");
    if (shard->count == shard->capacity && !shard_grow(shard))
        return false;
    XR_DCHECK(shard->capacity > 0, "shard_push_tail: zero capacity after grow");
    uint32_t idx = (shard->head + shard->count) % shard->capacity;
    shard->items[idx] = value;
    shard->count++;
    return true;
}

static bool shard_pop_tail(XrWorkQueueShard *shard, XrValue *out) {
    XR_DCHECK(shard != NULL, "shard_pop_tail: NULL shard");
    XR_DCHECK(out != NULL, "shard_pop_tail: NULL out");
    if (shard->count == 0)
        return false;
    uint32_t idx = (shard->head + shard->count - 1u) % shard->capacity;
    *out = shard->items[idx];
    shard->items[idx] = xr_null();
    shard->count--;
    if (shard->count == 0)
        shard->head = 0;
    return true;
}

static bool shard_pop_head(XrWorkQueueShard *shard, XrValue *out) {
    XR_DCHECK(shard != NULL, "shard_pop_head: NULL shard");
    XR_DCHECK(out != NULL, "shard_pop_head: NULL out");
    if (shard->count == 0)
        return false;
    *out = shard->items[shard->head];
    shard->items[shard->head] = xr_null();
    shard->head = (shard->head + 1u) % shard->capacity;
    shard->count--;
    if (shard->count == 0)
        shard->head = 0;
    return true;
}

static XrRuntime *work_queue_runtime(XrWorkQueue *q) {
    return q ? (XrRuntime *) q->scheduler : NULL;
}

static bool work_queue_runtime_can_wake(XrRuntime *runtime) {
    return runtime && runtime->workers && runtime->worker_count > 0;
}

static XrRuntime *work_queue_stats_runtime(XrWorkQueue *q) {
    XrRuntime *runtime = work_queue_runtime(q);
    return xr_sched_stats_enabled(runtime) ? runtime : NULL;
}

#define WORK_QUEUE_METRIC_INC(q, field)                                                            \
    do {                                                                                           \
        XrRuntime *_rt = work_queue_stats_runtime((q));                                            \
        if (_rt)                                                                                   \
            xr_sched_metric_inc(_rt, &_rt->sched_stats.field);                                     \
    } while (0)

#define WORK_QUEUE_METRIC_ADD(q, field, value)                                                     \
    do {                                                                                           \
        XrRuntime *_rt = work_queue_stats_runtime((q));                                            \
        if (_rt)                                                                                   \
            xr_sched_metric_add(_rt, &_rt->sched_stats.field, (uint64_t) (value));                 \
    } while (0)

static bool work_queue_waiter_enqueue_locked(XrWorkQueue *q, XrCoroutine *coro,
                                             int64_t worker_hint) {
    XR_DCHECK(q != NULL, "work_queue_waiter_enqueue_locked: NULL queue");
    XR_DCHECK(coro != NULL, "work_queue_waiter_enqueue_locked: NULL coro");
    XrCoroExt *ext = xr_coro_ensure_ext(coro);
    if (!ext)
        return false;

    ext->wait_link = NULL;
    ext->wait_prev = q->wait_last;
    ext->work_queue_hint = worker_hint;
    if (q->wait_last) {
        q->wait_last->ext->wait_link = coro;
    } else {
        q->wait_first = coro;
    }
    q->wait_last = coro;
    atomic_fetch_add_explicit(&q->waiter_count, 1, memory_order_relaxed);
    WORK_QUEUE_METRIC_INC(q, work_queue_block_count);
    return true;
}

static bool work_queue_pop_from_shard(XrWorkQueue *q, uint32_t shard_idx, bool own_shard,
                                      XrValue *out);

static bool work_queue_waiter_matches_shard(XrWorkQueue *q, XrCoroutine *coro, uint32_t shard_idx) {
    if (!q || !coro || !coro->ext || coro->ext->work_queue_hint < 0)
        return false;
    uint32_t wanted = (uint32_t) ((uint64_t) coro->ext->work_queue_hint % q->shard_count);
    return wanted == shard_idx;
}

static XrCoroutine *work_queue_waiter_pop_locked(XrWorkQueue *q) {
    XR_DCHECK(q != NULL, "work_queue_waiter_pop_locked: NULL queue");
    XrCoroutine *coro = q->wait_first;
    if (!coro)
        return NULL;

    xr_work_queue_waiter_unlink_locked(q, coro);
    return coro;
}

static XrCoroutine *work_queue_waiter_pop_for_shard_locked(XrWorkQueue *q, uint32_t shard_idx) {
    XR_DCHECK(q != NULL, "work_queue_waiter_pop_for_shard_locked: NULL queue");
    XrCoroutine *coro = q->wait_first;
    while (coro) {
        XrCoroutine *next = coro->ext ? coro->ext->wait_link : NULL;
        if (work_queue_waiter_matches_shard(q, coro, shard_idx)) {
            xr_work_queue_waiter_unlink_locked(q, coro);
            return coro;
        }
        coro = next;
    }
    return work_queue_waiter_pop_locked(q);
}

typedef struct XrWorkQueueWakeBatch {
    XrCoroutine *first;
    XrCoroutine *last;
    int count;
} XrWorkQueueWakeBatch;

static void work_queue_wake_batch_append(XrWorkQueueWakeBatch *batch, XrCoroutine *coro) {
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

static bool work_queue_claim_waiter(XrWorkQueue *q, XrCoroutine *coro, XrRuntime *runtime,
                                    int *target_id_out) {
    if (!work_queue_runtime_can_wake(runtime) || !coro || !target_id_out)
        return false;
    if (!xr_coro_claim_wake(coro))
        return false;

    WORK_QUEUE_METRIC_INC(q, work_queue_wake_count);
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (wait)
        xr_work_queue_wait_token_resolve(&wait->work_queue_token);
    xr_coro_resume_store(coro, XR_RESUME_OK);
    int target_id = xr_coro_wake_target_id(coro);
    if (target_id < 0 || target_id >= runtime->worker_count)
        target_id = 0;
    *target_id_out = target_id;
    return true;
}

static bool work_queue_ready_waiter(XrWorkQueue *q, XrCoroutine *coro) {
    XrRuntime *runtime = work_queue_runtime(q);
    int target_id = 0;
    if (!work_queue_claim_waiter(q, coro, runtime, &target_id))
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

static void work_queue_wake_one(XrWorkQueue *q, uint32_t shard_idx) {
    if (!q)
        return;
    if (!work_queue_runtime_can_wake(work_queue_runtime(q)))
        return;
    for (;;) {
        xr_amutex_lock(&q->wait_lock);
        XrCoroutine *coro = work_queue_waiter_pop_for_shard_locked(q, shard_idx);
        xr_amutex_unlock(&q->wait_lock);
        if (!coro)
            return;
        if (work_queue_ready_waiter(q, coro))
            return;
    }
}

static void work_queue_wake_all(XrWorkQueue *q) {
    if (!q)
        return;
    XrRuntime *runtime = work_queue_runtime(q);
    if (!work_queue_runtime_can_wake(runtime))
        return;

    xr_amutex_lock(&q->wait_lock);
    XrCoroutine *list = q->wait_first;
    q->wait_first = NULL;
    q->wait_last = NULL;
    atomic_store_explicit(&q->waiter_count, 0, memory_order_relaxed);
    xr_amutex_unlock(&q->wait_lock);

    XrWorker *current = xr_current_worker();
    bool current_matches = current && current->p.runtime == runtime;
    XrWorkQueueWakeBatch local_batch = {0};
    XrWorkQueueWakeBatch *remote_batches = NULL;
    if (runtime && runtime->worker_count > 0) {
        remote_batches = (XrWorkQueueWakeBatch *) xr_calloc((size_t) runtime->worker_count,
                                                            sizeof(XrWorkQueueWakeBatch));
    }
    int remote_ready_count = 0;

    while (list) {
        XrCoroutine *next = list->ext ? list->ext->wait_link : NULL;
        if (list->ext) {
            list->ext->wait_link = NULL;
            list->ext->wait_prev = NULL;
            list->ext->work_queue_hint = -1;
        }
        int target_id = 0;
        if (work_queue_claim_waiter(q, list, runtime, &target_id)) {
            WORK_QUEUE_METRIC_INC(q, work_queue_close_wake_count);
            if (current_matches && target_id == current->p.id) {
                work_queue_wake_batch_append(&local_batch, list);
            } else if (remote_batches) {
                work_queue_wake_batch_append(&remote_batches[target_id], list);
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
            XrWorkQueueWakeBatch *batch = &remote_batches[i];
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

static void work_queue_wake_some(XrWorkQueue *q, uint64_t max_count) {
    if (!q || max_count == 0)
        return;
    XrRuntime *runtime = work_queue_runtime(q);
    if (!work_queue_runtime_can_wake(runtime))
        return;

    XrCoroutine *list = NULL;
    XrCoroutine *last = NULL;
    uint64_t count = 0;

    xr_amutex_lock(&q->wait_lock);
    while (count < max_count && q->wait_first) {
        XrCoroutine *coro = work_queue_waiter_pop_locked(q);
        if (!coro)
            break;
        coro->sched_link = NULL;
        if (last) {
            last->sched_link = coro;
        } else {
            list = coro;
        }
        last = coro;
        count++;
    }
    xr_amutex_unlock(&q->wait_lock);

    XrWorker *current = xr_current_worker();
    bool current_matches = current && current->p.runtime == runtime;
    XrWorkQueueWakeBatch local_batch = {0};
    XrWorkQueueWakeBatch *remote_batches = NULL;
    if (runtime && runtime->worker_count > 0) {
        remote_batches = (XrWorkQueueWakeBatch *) xr_calloc((size_t) runtime->worker_count,
                                                            sizeof(XrWorkQueueWakeBatch));
    }
    int remote_ready_count = 0;

    while (list) {
        XrCoroutine *next = list->sched_link;
        list->sched_link = NULL;
        int target_id = 0;
        if (work_queue_claim_waiter(q, list, runtime, &target_id)) {
            if (current_matches && target_id == current->p.id) {
                work_queue_wake_batch_append(&local_batch, list);
            } else if (remote_batches) {
                work_queue_wake_batch_append(&remote_batches[target_id], list);
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
            XrWorkQueueWakeBatch *batch = &remote_batches[i];
            if (batch->first)
                xr_worker_inbox_enqueue_batch(runtime, i, batch->first, batch->last, batch->count);
        }
        xr_free(remote_batches);
    }
    if (remote_ready_count > 0 &&
        atomic_load_explicit(&runtime->spinning_count, memory_order_relaxed) == 0) {
        xr_runtime_wake_idle_worker(runtime);
    }
}

static XrValue work_queue_try_pop_raw(XrWorkQueue *q, int64_t worker_hint, bool *ok) {
    XR_DCHECK(q != NULL, "work_queue_try_pop_raw: NULL queue");
    XR_DCHECK(ok != NULL, "work_queue_try_pop_raw: NULL ok");

    uint32_t shard_count = q->shard_count;
    uint32_t owner;
    if (worker_hint >= 0) {
        owner = (uint32_t) ((uint64_t) worker_hint % shard_count);
    } else {
        owner = atomic_fetch_add_explicit(&q->next_shard, 1, memory_order_relaxed) % shard_count;
    }

    XrValue value = xr_null();
    if (work_queue_pop_from_shard(q, owner, true, &value)) {
        *ok = true;
        return value;
    }

    for (uint32_t i = 1; i < shard_count; i++) {
        uint32_t idx = (owner + i) % shard_count;
        if (work_queue_pop_from_shard(q, idx, false, &value)) {
            *ok = true;
            return value;
        }
    }

    *ok = false;
    WORK_QUEUE_METRIC_INC(q, work_queue_pop_empty_count);
    return xr_null();
}

XrWorkQueue *xr_work_queue_new(XrRuntimeCore *core, XrRuntime *scheduler, uint32_t shard_count,
                               uint32_t shard_capacity) {
    if (!core || !core->sys_heap)
        return NULL;
    if (shard_count == 0)
        shard_count = XR_WORK_QUEUE_DEFAULT_SHARDS;
    if (shard_capacity == 0)
        shard_capacity = XR_WORK_QUEUE_DEFAULT_CAPACITY;
    if (shard_count > XR_WORK_QUEUE_MAX_SHARDS)
        shard_count = XR_WORK_QUEUE_MAX_SHARDS;
    if (shard_capacity > XR_WORK_QUEUE_MAX_CAPACITY)
        shard_capacity = XR_WORK_QUEUE_MAX_CAPACITY;

    size_t alloc_size = sizeof(XrWorkQueue) + (size_t) shard_count * sizeof(XrWorkQueueShard);
    XrWorkQueue *q =
        (XrWorkQueue *) xr_sysheap_alloc_shared(core->sys_heap, alloc_size, XR_TWORKQUEUE);
    if (!q)
        return NULL;

    /* Atomic shared-RC like `shared const`: the compiler tracks the handle and
     * the last drop frees. NOT XR_OBJ_MANAGED (that no-ops drop -> leak). */
    xr_shared_set_refc(&q->hdr, 1);
    q->shard_count = shard_count;
    q->initial_capacity = shard_capacity;
    atomic_store_explicit(&q->next_shard, 0, memory_order_relaxed);
    atomic_store_explicit(&q->length, 0, memory_order_relaxed);
    atomic_store_explicit(&q->waiter_count, 0, memory_order_relaxed);
    atomic_store_explicit(&q->closed, false, memory_order_relaxed);
    xr_amutex_init(&q->wait_lock);
    q->wait_first = NULL;
    q->wait_last = NULL;
    q->core = core;
    q->scheduler = scheduler;

    for (uint32_t i = 0; i < shard_count; i++) {
        if (!shard_init(&q->shards[i], shard_capacity)) {
            for (uint32_t j = 0; j < i; j++)
                shard_reset(&q->shards[j]);
            xr_sysheap_free_shared(q, alloc_size);
            return NULL;
        }
    }
    return q;
}

bool xr_work_queue_push_core(XrRuntimeCore *core, XrWorkQueue *q, XrValue value,
                             int64_t shard_hint) {
    XR_DCHECK(q != NULL, "xr_work_queue_push: NULL queue");
    if (!core)
        core = q->core;
    if (!core)
        return false;
    if (atomic_load_explicit(&q->closed, memory_order_acquire))
        return false;

    uint32_t shard_count = q->shard_count;
    uint32_t shard_idx;
    if (shard_hint >= 0) {
        shard_idx = (uint32_t) ((uint64_t) shard_hint % shard_count);
    } else {
        shard_idx =
            atomic_fetch_add_explicit(&q->next_shard, 1, memory_order_relaxed) % shard_count;
    }

    value = xr_chan_prepare_send_core(core, value);
    XrWorkQueueShard *shard = &q->shards[shard_idx];
    bool ok;
    xr_amutex_lock(&shard->lock);
    if (atomic_load_explicit(&q->closed, memory_order_relaxed)) {
        ok = false;
    } else {
        ok = shard_push_tail(shard, value);
        if (ok)
            atomic_fetch_add_explicit(&q->length, 1, memory_order_release);
    }
    xr_amutex_unlock(&shard->lock);
    if (ok) {
        WORK_QUEUE_METRIC_INC(q, work_queue_push_count);
        work_queue_wake_one(q, shard_idx);
    } else {
        // Closed or push failure: the prepared value never entered the queue.
        xr_chan_abandon_send_core(core, value);
    }
    return ok;
}

int64_t xr_work_queue_push_int_range_core(XrRuntimeCore *core, XrWorkQueue *q, int64_t start,
                                          int64_t count, int64_t shard_start) {
    XR_DCHECK(q != NULL, "xr_work_queue_push_int_range: NULL queue");
    if (!core)
        core = q->core;
    if (!core)
        return 0;
    if (count <= 0)
        return 0;
    if (start > INT64_MAX - (count - 1))
        return 0;
    if (atomic_load_explicit(&q->closed, memory_order_acquire))
        return 0;

    uint32_t shard_count = q->shard_count;
    uint64_t base;
    if (shard_start >= 0) {
        base = (uint64_t) shard_start % shard_count;
    } else {
        base = atomic_fetch_add_explicit(&q->next_shard, (uint32_t) count, memory_order_relaxed) %
               shard_count;
    }

    uint64_t pushed = 0;
    bool ok = true;
    uint64_t total = (uint64_t) count;
    for (uint32_t s = 0; s < shard_count; s++) {
        uint64_t first = (s + shard_count - base) % shard_count;
        if (first >= total)
            continue;

        XrWorkQueueShard *shard = &q->shards[s];
        xr_amutex_lock(&shard->lock);
        if (atomic_load_explicit(&q->closed, memory_order_relaxed)) {
            ok = false;
        } else {
            for (uint64_t i = first; i < total; i += shard_count) {
                if (!shard_push_tail(shard, XR_FROM_INT(start + (int64_t) i))) {
                    ok = false;
                    break;
                }
                pushed++;
            }
        }
        xr_amutex_unlock(&shard->lock);
        if (!ok)
            break;
    }

    if (pushed > 0) {
        atomic_fetch_add_explicit(&q->length, pushed, memory_order_release);
        WORK_QUEUE_METRIC_ADD(q, work_queue_push_count, pushed);
        if (ok && pushed == total) {
            for (uint64_t i = 0; i < total; i++) {
                uint32_t shard_idx = (uint32_t) ((base + i) % shard_count);
                work_queue_wake_one(q, shard_idx);
            }
        } else {
            work_queue_wake_some(q, pushed);
        }
    }
    return pushed > (uint64_t) INT64_MAX ? INT64_MAX : (int64_t) pushed;
}

static bool work_queue_pop_from_shard(XrWorkQueue *q, uint32_t shard_idx, bool own_shard,
                                      XrValue *out) {
    XrWorkQueueShard *shard = &q->shards[shard_idx];
    bool ok;
    xr_amutex_lock(&shard->lock);
    ok = own_shard ? shard_pop_tail(shard, out) : shard_pop_head(shard, out);
    xr_amutex_unlock(&shard->lock);
    if (ok) {
        atomic_fetch_sub_explicit(&q->length, 1, memory_order_release);
        if (own_shard) {
            WORK_QUEUE_METRIC_INC(q, work_queue_pop_local_count);
        } else {
            WORK_QUEUE_METRIC_INC(q, work_queue_pop_steal_count);
        }
    }
    return ok;
}

XrValue xr_work_queue_try_pop_for_coro_core(XrRuntimeCore *core, XrWorkQueue *q,
                                            int64_t worker_hint, XrCoroutine *recv_coro, bool *ok) {
    XR_DCHECK(q != NULL, "xr_work_queue_try_pop: NULL queue");
    XR_DCHECK(ok != NULL, "xr_work_queue_try_pop: NULL ok");
    if (!core)
        core = q->core;
    if (!core) {
        *ok = false;
        return xr_null();
    }

    XrValue value = work_queue_try_pop_raw(q, worker_hint, ok);
    if (*ok)
        return xr_chan_copy_recv_core(core, value, recv_coro);
    return xr_null();
}

void xr_work_queue_close(XrWorkQueue *q) {
    if (!q)
        return;
    WORK_QUEUE_METRIC_INC(q, work_queue_close_count);
    atomic_store_explicit(&q->closed, true, memory_order_release);
    work_queue_wake_all(q);
}

bool xr_work_queue_is_closed(XrWorkQueue *q) {
    return q ? atomic_load_explicit(&q->closed, memory_order_acquire) : true;
}

uint64_t xr_work_queue_length(XrWorkQueue *q) {
    return q ? atomic_load_explicit(&q->length, memory_order_acquire) : 0;
}

static void work_queue_finish_wait_if_current(XrCoroutine *coro, XrWorkQueue *q) {
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return;
    void *current = atomic_load_explicit(&wait->work_queue_token.queue, memory_order_acquire);
    if (current == q)
        xr_work_queue_wait_token_finish(&wait->work_queue_token);
}

XrWorkQueuePopStatus xr_work_queue_pop_for_coro_core(XrRuntimeCore *core, XrWorkQueue *q,
                                                     XrCoroutine *coro, int64_t worker,
                                                     XrValue *result) {
    XR_DCHECK(q != NULL, "xr_work_queue_pop_for_coro: NULL queue");
    XR_DCHECK(result != NULL, "xr_work_queue_pop_for_coro: NULL result");
    if (!core)
        core = q->core;
    if (!core)
        return XR_WORK_QUEUE_POP_ERROR;

    bool ok = false;
    XrValue value = xr_work_queue_try_pop_for_coro_core(core, q, worker, coro, &ok);
    if (ok) {
        work_queue_finish_wait_if_current(coro, q);
        *result = value;
        return XR_WORK_QUEUE_POP_DONE;
    }
    if (xr_work_queue_is_closed(q)) {
        work_queue_finish_wait_if_current(coro, q);
        *result = xr_null();
        return XR_WORK_QUEUE_POP_DONE;
    }
    if (!coro)
        return XR_WORK_QUEUE_POP_ERROR;
    XrCoroWaitState *wait = xr_coro_ensure_wait_state(coro);
    if (!wait)
        return XR_WORK_QUEUE_POP_ERROR;
    xr_work_queue_wait_token_prepare(&wait->work_queue_token, q);

    xr_amutex_lock(&q->wait_lock);
    value = work_queue_try_pop_raw(q, worker, &ok);
    if (ok) {
        xr_amutex_unlock(&q->wait_lock);
        xr_work_queue_wait_token_finish(&wait->work_queue_token);
        *result = xr_chan_copy_recv_core(core, value, coro);
        return XR_WORK_QUEUE_POP_DONE;
    }
    if (xr_work_queue_is_closed(q)) {
        xr_amutex_unlock(&q->wait_lock);
        xr_work_queue_wait_token_finish(&wait->work_queue_token);
        *result = xr_null();
        return XR_WORK_QUEUE_POP_DONE;
    }

    xr_coro_set_wait_reason(coro, XR_CORO_WAIT_WORKQUEUE >> XR_CORO_WAIT_SHIFT);
    XrWorker *worker_state = xr_current_worker();
    if (worker_state)
        atomic_store_explicit(&coro->affinity_p, worker_state->p.id, memory_order_relaxed);
    if (!work_queue_waiter_enqueue_locked(q, coro, worker)) {
        xr_amutex_unlock(&q->wait_lock);
        xr_work_queue_wait_token_finish(&wait->work_queue_token);
        xr_coro_flags_clear(coro, XR_CORO_WAIT_MASK);
        return XR_WORK_QUEUE_POP_ERROR;
    }
    xr_work_queue_wait_token_commit(&wait->work_queue_token);
    (void) xr_coro_publish_wait_block(coro);
    xr_amutex_unlock(&q->wait_lock);
    return XR_WORK_QUEUE_POP_BLOCKED;
}

XrWorkQueuePopStatus xr_work_queue_pop_resume_for_coro_core(XrRuntimeCore *core, XrCoroutine *coro,
                                                            XrValue *result) {
    if (!coro || !result)
        return XR_WORK_QUEUE_POP_ERROR;
    XrCoroWaitState *wait = xr_coro_wait_state(coro);
    if (!wait)
        return XR_WORK_QUEUE_POP_ERROR;
    XrWorkQueue *q =
        (XrWorkQueue *) atomic_load_explicit(&wait->work_queue_token.queue, memory_order_acquire);
    if (!q)
        return XR_WORK_QUEUE_POP_ERROR;
    int64_t worker = coro->ext ? coro->ext->work_queue_hint : -1;
    return xr_work_queue_pop_for_coro_core(core ? core : q->core, q, coro, worker, result);
}

#undef WORK_QUEUE_METRIC_INC
#undef WORK_QUEUE_METRIC_ADD
