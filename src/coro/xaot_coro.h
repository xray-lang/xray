/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_coro.h - AOT coroutine runtime bridge
 */

#ifndef XAOT_CORO_H
#define XAOT_CORO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xslot_ref.h"

#ifndef XR_FUNC
#define XR_FUNC extern
#endif

#ifndef XRT_VALUE_H
#include "../runtime/value/xvalue.h"
#endif

struct XrayIsolate;
struct XrCoroutine;
struct XrTask;

typedef enum {
    XR_AOT_RUN_DONE = 0,
    XR_AOT_RUN_BLOCKED,
    XR_AOT_RUN_YIELD,
    XR_AOT_RUN_SPAWN_CHILD,
    XR_AOT_RUN_ERROR,
    XR_AOT_RUN_CANCELLED
} XrAotRunKind;

typedef struct XrAotResult {
    XrAotRunKind kind;
    XrValue value;
    XrValue error;
    struct XrCoroutine *child;
    bool error_is_value;
} XrAotResult;

typedef struct XrAotContext {
    struct XrCoroutine *coro;
    struct XrayIsolate *isolate;
    void *worker;
} XrAotContext;

typedef XrAotResult (*XrAotResumeFn)(void *frame, const XrAotContext *ctx);
typedef void (*XrAotFrameTraceFn)(void *frame, void *visitor);
typedef void (*XrAotFrameReleaseFn)(void *frame);
typedef void (*XrAotRootVisitFn)(XrValue value, void *ctx);

typedef struct XrAotRootVisitor {
    XrAotRootVisitFn visit;
    void *ctx;
} XrAotRootVisitor;

typedef struct XrAotCoroDesc {
    const char *name;
    size_t frame_size;
    XrAotResumeFn resume;
    XrAotFrameTraceFn trace_roots;
    XrAotFrameReleaseFn release_frame;
} XrAotCoroDesc;

typedef struct XrAotSpawnResult {
    XrValue task_value;
    struct XrCoroutine *child;
} XrAotSpawnResult;

static inline XrAotResult xr_aot_result(XrAotRunKind kind) {
    XrAotResult result;
    result.kind = kind;
    result.value = XR_NULL_VAL;
    result.error = XR_NULL_VAL;
    result.child = NULL;
    result.error_is_value = false;
    return result;
}

static inline XrAotResult xr_aot_done(XrValue value) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_DONE);
    result.value = value;
    return result;
}

static inline XrAotResult xr_aot_blocked(void) {
    return xr_aot_result(XR_AOT_RUN_BLOCKED);
}

static inline XrAotResult xr_aot_yielded(void) {
    return xr_aot_result(XR_AOT_RUN_YIELD);
}

static inline XrAotResult xr_aot_spawn_child(struct XrCoroutine *child) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_SPAWN_CHILD);
    result.child = child;
    return result;
}

static inline XrAotResult xr_aot_error(XrValue error, bool error_is_value) {
    XrAotResult result = xr_aot_result(XR_AOT_RUN_ERROR);
    result.error = error;
    result.error_is_value = error_is_value;
    return result;
}

XR_FUNC void *xr_aot_frame_alloc(size_t size);
XR_FUNC void xr_aot_frame_free(void *frame);
XR_FUNC void xr_aot_trace_frame_value(void *visitor, XrValue value);
XR_FUNC void xr_aot_release_frame_value(XrValue value);

XR_FUNC struct XrCoroutine *xr_coro_create_aot(struct XrayIsolate *X, const XrAotCoroDesc *desc,
                                               void *frame, const char *name);

XR_FUNC XrValue xr_aot_run_main(struct XrayIsolate *X, const XrAotCoroDesc *desc, void *frame);

XR_FUNC XrAotSpawnResult xr_aot_spawn(const XrAotContext *ctx, const XrAotCoroDesc *desc,
                                      void *frame, int priority, int link_mode,
                                      bool fire_and_forget, const char *name);

XR_FUNC XrAotResult xr_aot_sleep(const XrAotContext *ctx, int64_t milliseconds);
XR_FUNC XrAotResult xr_aot_await_task(const XrAotContext *ctx, XrValue task_value,
                                      XrValue *out_value, bool discard_result);
XR_FUNC XrAotResult xr_aot_await_task_resume(const XrAotContext *ctx, XrValue task_value,
                                             XrValue *out_value, bool discard_result);

XR_FUNC XrValue xr_aot_channel_new(const XrAotContext *ctx, int64_t buffer_size);
XR_FUNC XrValue xr_aot_chan_try_send(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue send_value);
XR_FUNC XrValue xr_aot_chan_try_recv(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_close(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_chan_is_closed(const XrAotContext *ctx, XrValue channel_value);
XR_FUNC XrValue xr_aot_tuple_get(const XrAotContext *ctx, XrValue tuple_value, uint16_t index);
XR_FUNC XrAotResult xr_aot_chan_send(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue send_value);
XR_FUNC XrAotResult xr_aot_chan_send_resume(const XrAotContext *ctx);
XR_FUNC XrAotResult xr_aot_chan_recv(const XrAotContext *ctx, XrValue channel_value,
                                     XrValue *out_value);
XR_FUNC XrAotResult xr_aot_chan_recv_resume(const XrAotContext *ctx, XrValue *out_value);
XR_FUNC XrAotResult xr_aot_chan_recv_slot(const XrAotContext *ctx, XrValue channel_value,
                                          XrSlotRef out_slot);
XR_FUNC XrAotResult xr_aot_chan_recv_slot_resume(const XrAotContext *ctx, XrSlotRef out_slot);

#endif  // XAOT_CORO_H
