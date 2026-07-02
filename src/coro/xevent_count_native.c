/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xevent_count_native.c - VM native EventCount methods and registration
 */

#include "xevent_count.h"

#include "../base/xchecks.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "xcoroutine.h"
#include "xyieldable.h"

static XrValue m_advance(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrEventCount *event = xr_value_to_event_count(self);
    XR_DCHECK(event != NULL, "EventCount.advance: NULL event");
    int64_t step = nargs >= 1 && XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 1;
    return xr_int(xr_event_count_advance(event, step));
}

static XrCFuncResult ym_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs,
                             XrValue *result) {
    XrEventCount *event = xr_value_to_event_count(self);
    XR_DCHECK(event != NULL, "EventCount.wait: NULL event");
    XR_DCHECK(result != NULL, "EventCount.wait: NULL result");
    int64_t last_epoch = nargs >= 1 && XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 0;
    int64_t worker_hint = nargs >= 2 && XR_IS_INT(args[1]) ? XR_TO_INT(args[1]) : -1;

    int64_t epoch = -1;
    switch (xr_event_count_wait_for_coro(event, xr_current_coro(isolate), last_epoch, worker_hint,
                                         &epoch)) {
        case XR_EVENT_COUNT_WAIT_CHANGED:
        case XR_EVENT_COUNT_WAIT_CLOSED:
            *result = xr_int(epoch);
            return XR_CFUNC_DONE;
        case XR_EVENT_COUNT_WAIT_BLOCKED:
            return XR_CFUNC_BLOCKED;
        case XR_EVENT_COUNT_WAIT_ERROR:
        default:
            return XR_CFUNC_ERROR;
    }
}

static XrValue m_close(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    xr_event_count_close(xr_value_to_event_count(self));
    return xr_null();
}

static XrValue g_epoch(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(xr_event_count_epoch(xr_value_to_event_count(self)));
}

static XrValue g_is_closed(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_event_count_is_closed(xr_value_to_event_count(self)));
}

static XrValue event_count_construct(XrVMRuntime *isolate, XrValue receiver, XrValue *args,
                                     int nargs) {
    (void) receiver;
    int64_t epoch = 0;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        epoch = XR_TO_INT(args[0]);

    XrEventCount *event = xr_event_count_new(xr_isolate_get_runtime_core(isolate),
                                             xr_isolate_get_scheduler_runtime(isolate), epoch);
    if (!event) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "EventCount allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_event_count(event);
}

void xr_event_count_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod event_count_methods[] = {
        {"advance", m_advance, 0},
        {"close", m_close, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeYieldableMethod event_count_yieldable_methods[] = {
        {"wait", ym_wait, 1},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod event_count_getters[] = {
        {"epoch", g_epoch, 0},
        {"isClosed", g_is_closed, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod event_count_statics[] = {
        {"call", event_count_construct, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo info = {
        .name = "EventCount",
        .gc_type = XR_TEVENTCOUNT,
        .methods = (XrNativeMethod *) event_count_methods,
        .yieldable_methods = (XrNativeYieldableMethod *) event_count_yieldable_methods,
        .getters = (XrNativeMethod *) event_count_getters,
        .static_methods = (XrNativeMethod *) event_count_statics,
    };
    xr_register_native_type(isolate, &info);
}
