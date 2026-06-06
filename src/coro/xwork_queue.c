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
#include "../runtime/gc/xsystem_heap.h"
#include "../runtime/object/xexception.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/xshared.h"
#include "../vm/xvm.h"
#include "xchannel_ops.h"
#include "xcoroutine.h"

#define XR_WORK_QUEUE_DEFAULT_SHARDS 1u
#define XR_WORK_QUEUE_DEFAULT_CAPACITY 64u
#define XR_WORK_QUEUE_MAX_SHARDS 65536u
#define XR_WORK_QUEUE_MAX_CAPACITY (1u << 30)

static uint32_t sanitize_shard_count(int64_t value) {
    if (value <= 0)
        return XR_WORK_QUEUE_DEFAULT_SHARDS;
    if (value > XR_WORK_QUEUE_MAX_SHARDS)
        return XR_WORK_QUEUE_MAX_SHARDS;
    return (uint32_t) value;
}

static uint32_t sanitize_capacity(int64_t value) {
    if (value <= 0)
        return XR_WORK_QUEUE_DEFAULT_CAPACITY;
    if (value > XR_WORK_QUEUE_MAX_CAPACITY)
        return XR_WORK_QUEUE_MAX_CAPACITY;
    return (uint32_t) value;
}

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

static void shard_destroy(XrWorkQueueShard *shard) {
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

XrWorkQueue *xr_work_queue_new(XrayIsolate *X, uint32_t shard_count, uint32_t shard_capacity) {
    if (!X || !xr_isolate_get_sys_heap(X))
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
    XrWorkQueue *q = (XrWorkQueue *) xr_sysheap_alloc_shared(xr_isolate_get_sys_heap(X), alloc_size,
                                                             XR_TWORKQUEUE);
    if (!q)
        return NULL;

    xr_shared_set_refc(&q->gc, 1);
    XR_OBJ_SET_FLAG(&q->gc, XR_OBJ_MANAGED);
    q->shard_count = shard_count;
    q->initial_capacity = shard_capacity;
    atomic_store_explicit(&q->next_shard, 0, memory_order_relaxed);
    atomic_store_explicit(&q->length, 0, memory_order_relaxed);
    atomic_store_explicit(&q->closed, false, memory_order_relaxed);
    q->isolate = X;

    for (uint32_t i = 0; i < shard_count; i++) {
        if (!shard_init(&q->shards[i], shard_capacity)) {
            for (uint32_t j = 0; j < i; j++)
                shard_destroy(&q->shards[j]);
            xr_sysheap_free_shared(q, alloc_size);
            return NULL;
        }
    }
    return q;
}

bool xr_work_queue_push(XrayIsolate *X, XrWorkQueue *q, XrValue value, int64_t shard_hint) {
    XR_DCHECK(q != NULL, "xr_work_queue_push: NULL queue");
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

    value = xr_chan_prepare_send(X, value);
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
    return ok;
}

static bool work_queue_pop_from_shard(XrWorkQueue *q, uint32_t shard_idx, bool own_shard,
                                      XrValue *out) {
    XrWorkQueueShard *shard = &q->shards[shard_idx];
    bool ok;
    xr_amutex_lock(&shard->lock);
    ok = own_shard ? shard_pop_tail(shard, out) : shard_pop_head(shard, out);
    xr_amutex_unlock(&shard->lock);
    if (ok)
        atomic_fetch_sub_explicit(&q->length, 1, memory_order_release);
    return ok;
}

XrValue xr_work_queue_try_pop(XrayIsolate *X, XrWorkQueue *q, int64_t worker_hint, bool *ok) {
    XR_DCHECK(q != NULL, "xr_work_queue_try_pop: NULL queue");
    XR_DCHECK(ok != NULL, "xr_work_queue_try_pop: NULL ok");

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
        return xr_chan_copy_recv(X, value, xr_current_coro(X));
    }

    for (uint32_t i = 1; i < shard_count; i++) {
        uint32_t idx = (owner + i) % shard_count;
        if (work_queue_pop_from_shard(q, idx, false, &value)) {
            *ok = true;
            return xr_chan_copy_recv(X, value, xr_current_coro(X));
        }
    }

    *ok = false;
    return xr_null();
}

void xr_work_queue_close(XrWorkQueue *q) {
    if (!q)
        return;
    atomic_store_explicit(&q->closed, true, memory_order_release);
}

bool xr_work_queue_is_closed(XrWorkQueue *q) {
    return q ? atomic_load_explicit(&q->closed, memory_order_acquire) : true;
}

uint64_t xr_work_queue_length(XrWorkQueue *q) {
    return q ? atomic_load_explicit(&q->length, memory_order_acquire) : 0;
}

void xr_gc_destroy_work_queue(XrGCHeader *obj, XrCoroGC *owning_gc) {
    (void) owning_gc;
    if (!obj)
        return;
    XrWorkQueue *q = (XrWorkQueue *) obj;
    for (uint32_t i = 0; i < q->shard_count; i++)
        shard_destroy(&q->shards[i]);
}

static XrValue m_push(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XrWorkQueue *q = xr_value_to_work_queue(self);
    XR_DCHECK(q != NULL, "WorkQueue.push: NULL queue");
    XR_DCHECK(nargs >= 1, "WorkQueue.push: missing value");
    int64_t shard = -1;
    if (nargs >= 2 && XR_IS_INT(args[1]))
        shard = XR_TO_INT(args[1]);
    return xr_bool(xr_work_queue_push(isolate, q, args[0], shard));
}

static XrValue m_try_pop(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    XrWorkQueue *q = xr_value_to_work_queue(self);
    XR_DCHECK(q != NULL, "WorkQueue.tryPop: NULL queue");
    int64_t worker = -1;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        worker = XR_TO_INT(args[0]);

    bool ok = false;
    XrValue value = xr_work_queue_try_pop(isolate, q, worker, &ok);
    XrTuple *tuple = xr_tuple_new(xr_current_coro(isolate), 2);
    if (!tuple)
        return xr_null();
    xr_tuple_set(tuple, 0, value);
    xr_tuple_set(tuple, 1, xr_bool(ok));
    return xr_value_from_tuple(tuple);
}

static XrValue m_close(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    xr_work_queue_close(xr_value_to_work_queue(self));
    return xr_null();
}

static XrValue g_length(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) xr_work_queue_length(xr_value_to_work_queue(self)));
}

static XrValue g_shard_count(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    XrWorkQueue *q = xr_value_to_work_queue(self);
    return xr_int(q ? (int64_t) q->shard_count : 0);
}

static XrValue g_is_closed(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_work_queue_is_closed(xr_value_to_work_queue(self)));
}

static XrValue work_queue_construct(XrayIsolate *isolate, XrValue receiver, XrValue *args,
                                    int nargs) {
    (void) receiver;
    uint32_t shards = XR_WORK_QUEUE_DEFAULT_SHARDS;
    uint32_t capacity = XR_WORK_QUEUE_DEFAULT_CAPACITY;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        shards = sanitize_shard_count(XR_TO_INT(args[0]));
    if (nargs >= 2 && XR_IS_INT(args[1]))
        capacity = sanitize_capacity(XR_TO_INT(args[1]));

    XrWorkQueue *q = xr_work_queue_new(isolate, shards, capacity);
    if (!q) {
        XrValue exc =
            xr_exception_newf(isolate, XR_ERR_OUT_OF_MEMORY, "WorkQueue allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_work_queue(q);
}

void xr_work_queue_register_native_type(XrayIsolate *isolate) {
    static const XrNativeMethod work_queue_methods[] = {
        {"push", m_push, 1},
        {"tryPop", m_try_pop, 0},
        {"close", m_close, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod work_queue_getters[] = {
        {"length", g_length, 0},
        {"shardCount", g_shard_count, 0},
        {"isClosed", g_is_closed, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod work_queue_statics[] = {
        {"call", work_queue_construct, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo info = {
        .name = "WorkQueue",
        .gc_type = XR_TWORKQUEUE,
        .methods = (XrNativeMethod *) work_queue_methods,
        .getters = (XrNativeMethod *) work_queue_getters,
        .static_methods = (XrNativeMethod *) work_queue_statics,
    };
    xr_register_native_type(isolate, &info);
}
