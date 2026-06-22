/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresult_group.h - Shared scalar result reducer primitive
 */

#ifndef XRESULT_GROUP_H
#define XRESULT_GROUP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#include "../base/xdefs.h"
#include "../base/xmutex.h"
#include "../runtime/gc/xobj_header.h"
#include "../runtime/value/xvalue.h"

struct XrayIsolate;
struct XrCoroHeap;
struct XrCoroutine;
struct XrRuntime;
struct XrRuntimeCore;

#define XR_RESULT_GROUP_DEFAULT_BATCH 64u
#define XR_RESULT_GROUP_MAX_BATCH (1u << 20)

typedef enum XrResultGroupRecvStatus {
    XR_RESULT_GROUP_RECV_DONE = 0,
    XR_RESULT_GROUP_RECV_BLOCKED,
    XR_RESULT_GROUP_RECV_ERROR
} XrResultGroupRecvStatus;

typedef struct XrResultGroupBatch {
    int64_t value;
    uint32_t count;
    struct XrResultGroupBatch *next;
} XrResultGroupBatch;

typedef struct XrResultGroup {
    XrObjHeader hdr;
    XrAdaptiveMutex lock;
    _Atomic(uint64_t) length;
    _Atomic(uint64_t) pending_count;
    _Atomic(uint64_t) waiter_count;
    _Atomic(bool) closed;
    uint32_t batch_size;
    int64_t current_value;
    uint32_t current_count;
    XrResultGroupBatch *batch_first;
    XrResultGroupBatch *batch_last;
    struct XrCoroutine *wait_first;
    struct XrCoroutine *wait_last;
    struct XrRuntimeCore *core;
    struct XrRuntime *scheduler;
} XrResultGroup;

XR_FUNC XrResultGroup *xr_result_group_new(struct XrRuntimeCore *core, struct XrRuntime *scheduler,
                                           uint32_t batch_size);
XR_FUNC bool xr_result_group_add(XrResultGroup *g, int64_t value);
XR_FUNC bool xr_result_group_flush(XrResultGroup *g);
XR_FUNC bool xr_result_group_try_recv(XrResultGroup *g, int64_t *out);
XR_FUNC XrResultGroupRecvStatus xr_result_group_recv_for_coro(XrResultGroup *g,
                                                              struct XrCoroutine *coro,
                                                              XrValue *result);
XR_FUNC XrResultGroupRecvStatus xr_result_group_recv_resume_for_coro(struct XrCoroutine *coro,
                                                                     XrValue *result);
XR_FUNC void xr_result_group_cancel_waiter(struct XrCoroutine *coro);
XR_FUNC void xr_result_group_close(XrResultGroup *g);
XR_FUNC bool xr_result_group_is_closed(XrResultGroup *g);
XR_FUNC uint64_t xr_result_group_length(XrResultGroup *g);
XR_FUNC uint64_t xr_result_group_pending_count(XrResultGroup *g);
XR_FUNC void xr_obj_destroy_result_group(XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_result_group_register_native_type(struct XrayIsolate *X);

static inline bool xr_value_is_result_group(XrValue v) {
    return XR_IS_PTR(v) && XR_HEAP_TYPE(v) == XR_TRESULTGROUP;
}

static inline XrResultGroup *xr_value_to_result_group(XrValue v) {
    return (XrResultGroup *) XR_TO_PTR(v);
}

static inline XrValue xr_value_from_result_group(XrResultGroup *g) {
    return XR_FROM_PTR(g);
}

#endif  // XRESULT_GROUP_H
