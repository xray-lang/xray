/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcountdown_latch.h - Shared countdown barrier primitive
 */

#ifndef XCOUNTDOWN_LATCH_H
#define XCOUNTDOWN_LATCH_H

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

typedef enum XrCountdownLatchWaitStatus {
    XR_COUNTDOWN_LATCH_WAIT_DONE = 0,
    XR_COUNTDOWN_LATCH_WAIT_BLOCKED,
    XR_COUNTDOWN_LATCH_WAIT_CLOSED,
    XR_COUNTDOWN_LATCH_WAIT_ERROR
} XrCountdownLatchWaitStatus;

typedef struct XrCountdownLatch {
    XrObjHeader hdr;
    XrAdaptiveMutex lock;
    _Atomic(int64_t) remaining;
    _Atomic(uint64_t) waiter_count;
    _Atomic(bool) closed;
    struct XrCoroutine *wait_first;
    struct XrCoroutine *wait_last;
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;
} XrCountdownLatch;

XR_FUNC XrCountdownLatch *xr_countdown_latch_new(struct XrRuntimeCore *core,
                                                 struct XrRuntime *scheduler, int64_t count);
XR_FUNC bool xr_countdown_latch_reset(XrCountdownLatch *latch, int64_t count);
XR_FUNC int64_t xr_countdown_latch_done(XrCountdownLatch *latch, int64_t count);
XR_FUNC bool xr_countdown_latch_try_wait(XrCountdownLatch *latch);
XR_FUNC XrCountdownLatchWaitStatus xr_countdown_latch_wait_for_coro(XrCountdownLatch *latch,
                                                                    struct XrCoroutine *coro,
                                                                    bool *result);
XR_FUNC XrCountdownLatchWaitStatus xr_countdown_latch_wait_resume_for_coro(struct XrCoroutine *coro,
                                                                           bool *result);
XR_FUNC void xr_countdown_latch_cancel_waiter(struct XrCoroutine *coro);
XR_FUNC void xr_countdown_latch_close(XrCountdownLatch *latch);
XR_FUNC bool xr_countdown_latch_is_closed(XrCountdownLatch *latch);
XR_FUNC int64_t xr_countdown_latch_remaining(XrCountdownLatch *latch);
XR_FUNC void xr_obj_destroy_countdown_latch(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_countdown_latch_register_native_type(struct XrVMRuntime *X);

static inline bool xr_value_is_countdown_latch(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TCOUNTDOWNLATCH;
}

static inline XrCountdownLatch *xr_value_to_countdown_latch(XrValue v) {
    return (XrCountdownLatch *) XR_TO_PTR(v);
}

static inline XrValue xr_value_from_countdown_latch(XrCountdownLatch *latch) {
    return XR_FROM_PTR(latch);
}

#endif  // XCOUNTDOWN_LATCH_H
