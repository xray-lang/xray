/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xevent_count.h - Shared epoch/event-count primitive
 */

#ifndef XEVENT_COUNT_H
#define XEVENT_COUNT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "../base/xdefs.h"
#include "../base/xmutex.h"
#include "../runtime/mem/xobj_header.h"
#include "../runtime/value/xvalue.h"

struct XrVMRuntime;
struct XrCoroHeap;
struct XrCoroutine;
struct XrRuntime;
struct XrRuntimeCore;

typedef enum XrEventCountWaitStatus {
    XR_EVENT_COUNT_WAIT_CHANGED = 0,
    XR_EVENT_COUNT_WAIT_BLOCKED,
    XR_EVENT_COUNT_WAIT_CLOSED,
    XR_EVENT_COUNT_WAIT_ERROR
} XrEventCountWaitStatus;

typedef struct XrEventCount {
    XrObjHeader hdr;
    XrAdaptiveMutex lock;
    _Atomic(int64_t) epoch;
    _Atomic(uint64_t) waiter_count;
    _Atomic(bool) closed;
    struct XrCoroutine *wait_first;
    struct XrCoroutine *wait_last;
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;
} XrEventCount;

XR_FUNC XrEventCount *xr_event_count_new(struct XrRuntimeCore *core, struct XrRuntime *scheduler,
                                         int64_t epoch);
XR_FUNC int64_t xr_event_count_advance(XrEventCount *event, int64_t step);
XR_FUNC XrEventCountWaitStatus xr_event_count_wait_for_coro(XrEventCount *event,
                                                            struct XrCoroutine *coro,
                                                            int64_t last_epoch, int64_t worker_hint,
                                                            int64_t *result_epoch);
XR_FUNC XrEventCountWaitStatus xr_event_count_wait_resume_for_coro(struct XrCoroutine *coro,
                                                                   int64_t *result_epoch);
XR_FUNC void xr_event_count_cancel_waiter(struct XrCoroutine *coro);
XR_FUNC void xr_event_count_close(XrEventCount *event);
XR_FUNC bool xr_event_count_is_closed(XrEventCount *event);
XR_FUNC int64_t xr_event_count_epoch(XrEventCount *event);
XR_FUNC void xr_obj_destroy_event_count(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_event_count_register_native_type(struct XrVMRuntime *X);

static inline bool xr_value_is_event_count(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TEVENTCOUNT;
}

static inline XrEventCount *xr_value_to_event_count(XrValue v) {
    return (XrEventCount *) XR_TO_PTR(v);
}

static inline XrValue xr_value_from_event_count(XrEventCount *event) {
    return XR_FROM_PTR(event);
}

#endif  // XEVENT_COUNT_H
