/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xwork_queue_native.c - VM native WorkQueue methods and registration
 */

#include "xwork_queue.h"

#include "../base/xchecks.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "xcoroutine.h"
#include "xyieldable.h"

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

bool xr_work_queue_push(XrayIsolate *X, XrWorkQueue *q, XrValue value, int64_t shard_hint) {
    return xr_work_queue_push_core(X ? xr_isolate_get_runtime_core(X) : NULL, q, value, shard_hint);
}

XrValue xr_work_queue_try_pop(XrayIsolate *X, XrWorkQueue *q, int64_t worker_hint, bool *ok) {
    return xr_work_queue_try_pop_for_coro_core(X ? xr_isolate_get_runtime_core(X) : NULL, q,
                                               worker_hint, X ? xr_current_coro(X) : NULL, ok);
}

XrWorkQueuePopStatus xr_work_queue_pop_for_coro(XrayIsolate *isolate, XrWorkQueue *q,
                                                XrCoroutine *coro, int64_t worker,
                                                XrValue *result) {
    return xr_work_queue_pop_for_coro_core(isolate ? xr_isolate_get_runtime_core(isolate) : NULL, q,
                                           coro, worker, result);
}

XrWorkQueuePopStatus xr_work_queue_pop_resume_for_coro(XrayIsolate *isolate, XrCoroutine *coro,
                                                       XrValue *result) {
    return xr_work_queue_pop_resume_for_coro_core(
        isolate ? xr_isolate_get_runtime_core(isolate) : NULL, coro, result);
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

static XrCFuncResult ym_pop(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs,
                            XrValue *result) {
    XrWorkQueue *q = xr_value_to_work_queue(self);
    XR_DCHECK(q != NULL, "WorkQueue.pop: NULL queue");
    XR_DCHECK(result != NULL, "WorkQueue.pop: NULL result");
    int64_t worker = -1;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        worker = XR_TO_INT(args[0]);

    switch (xr_work_queue_pop_for_coro(isolate, q, xr_current_coro(isolate), worker, result)) {
        case XR_WORK_QUEUE_POP_DONE:
            return XR_CFUNC_DONE;
        case XR_WORK_QUEUE_POP_BLOCKED:
            return XR_CFUNC_BLOCKED;
        case XR_WORK_QUEUE_POP_WOULD_BLOCK:
            return XR_CFUNC_WOULD_BLOCK;
        case XR_WORK_QUEUE_POP_ERROR:
        default:
            return XR_CFUNC_ERROR;
    }
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

    XrWorkQueue *q = xr_work_queue_new(xr_isolate_get_runtime_core(isolate),
                                       xr_isolate_get_scheduler_runtime(isolate), shards, capacity);
    if (!q) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "WorkQueue allocation failed");
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
    static const XrNativeYieldableMethod work_queue_yieldable_methods[] = {
        {"pop", ym_pop, 0},
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
        .yieldable_methods = (XrNativeYieldableMethod *) work_queue_yieldable_methods,
        .getters = (XrNativeMethod *) work_queue_getters,
        .static_methods = (XrNativeMethod *) work_queue_statics,
    };
    xr_register_native_type(isolate, &info);
}
