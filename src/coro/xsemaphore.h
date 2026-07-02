/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemaphore.h - Shared counting semaphore primitive
 */

#ifndef XSEMAPHORE_H
#define XSEMAPHORE_H

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

typedef enum XrSemaphoreWaitStatus {
    XR_SEMAPHORE_WAIT_ACQUIRED = 0,
    XR_SEMAPHORE_WAIT_BLOCKED,
    XR_SEMAPHORE_WAIT_CLOSED,
    XR_SEMAPHORE_WAIT_ERROR
} XrSemaphoreWaitStatus;

typedef struct XrSemaphore {
    XrObjHeader hdr;
    XrAdaptiveMutex lock;
    _Atomic(int64_t) available;
    _Atomic(uint64_t) waiter_count;
    _Atomic(bool) closed;
    struct XrCoroutine *wait_first;
    struct XrCoroutine *wait_last;
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;
} XrSemaphore;

XR_FUNC XrSemaphore *xr_semaphore_new(struct XrRuntimeCore *core, struct XrRuntime *scheduler,
                                      int64_t permits);
XR_FUNC int64_t xr_semaphore_release(XrSemaphore *sem, int64_t count);
XR_FUNC bool xr_semaphore_try_acquire(XrSemaphore *sem);
XR_FUNC XrSemaphoreWaitStatus xr_semaphore_acquire_for_coro(XrSemaphore *sem,
                                                            struct XrCoroutine *coro, bool *result);
XR_FUNC XrSemaphoreWaitStatus xr_semaphore_acquire_resume_for_coro(struct XrCoroutine *coro,
                                                                   bool *result);
XR_FUNC void xr_semaphore_cancel_waiter(struct XrCoroutine *coro);
XR_FUNC void xr_semaphore_close(XrSemaphore *sem);
XR_FUNC bool xr_semaphore_is_closed(XrSemaphore *sem);
XR_FUNC int64_t xr_semaphore_available(XrSemaphore *sem);
XR_FUNC void xr_obj_destroy_semaphore(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_semaphore_register_native_type(struct XrVMRuntime *X);

static inline bool xr_value_is_semaphore(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TSEMAPHORE;
}

static inline XrSemaphore *xr_value_to_semaphore(XrValue v) {
    return (XrSemaphore *) XR_TO_PTR(v);
}

static inline XrValue xr_value_from_semaphore(XrSemaphore *sem) {
    return XR_FROM_PTR(sem);
}

#endif  // XSEMAPHORE_H
