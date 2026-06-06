/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwork_queue.h - Shared sharded work queue primitive
 */

#ifndef XWORK_QUEUE_H
#define XWORK_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "../base/xdefs.h"
#include "../base/xmutex.h"
#include "../runtime/gc/xgc_header.h"
#include "../runtime/value/xvalue.h"

struct XrayIsolate;
struct XrCoroGC;
struct XrCoroutine;

typedef struct XrWorkQueueShard {
    XrAdaptiveMutex lock;
    XrValue *items;
    uint32_t capacity;
    uint32_t head;
    uint32_t count;
} XrWorkQueueShard;

typedef struct XrWorkQueue {
    XrGCHeader gc;
    uint32_t shard_count;
    uint32_t initial_capacity;
    _Atomic(uint32_t) next_shard;
    _Atomic(uint64_t) length;
    _Atomic(uint64_t) waiter_count;
    _Atomic(bool) closed;
    XrAdaptiveMutex wait_lock;
    struct XrCoroutine *wait_first;
    struct XrCoroutine *wait_last;
    struct XrayIsolate *isolate;
    XrWorkQueueShard shards[];
} XrWorkQueue;

XR_FUNC XrWorkQueue *xr_work_queue_new(struct XrayIsolate *X, uint32_t shard_count,
                                       uint32_t shard_capacity);
XR_FUNC bool xr_work_queue_push(struct XrayIsolate *X, XrWorkQueue *q, XrValue value,
                                int64_t shard_hint);
XR_FUNC XrValue xr_work_queue_try_pop(struct XrayIsolate *X, XrWorkQueue *q, int64_t worker_hint,
                                      bool *ok);
XR_FUNC void xr_work_queue_cancel_waiter(struct XrCoroutine *coro);
XR_FUNC void xr_work_queue_close(XrWorkQueue *q);
XR_FUNC bool xr_work_queue_is_closed(XrWorkQueue *q);
XR_FUNC uint64_t xr_work_queue_length(XrWorkQueue *q);
XR_FUNC void xr_gc_destroy_work_queue(XrGCHeader *obj, struct XrCoroGC *owning_gc);
XR_FUNC void xr_work_queue_register_native_type(struct XrayIsolate *X);

static inline bool xr_value_is_work_queue(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TWORKQUEUE;
}

static inline XrWorkQueue *xr_value_to_work_queue(XrValue v) {
    return (XrWorkQueue *) XR_TO_PTR(v);
}

static inline XrValue xr_value_from_work_queue(XrWorkQueue *q) {
    return XR_FROM_PTR(q);
}

#endif  // XWORK_QUEUE_H
