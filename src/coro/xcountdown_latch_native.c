/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcountdown_latch_native.c - VM native CountdownLatch methods and registration
 */

#include "xcountdown_latch.h"

#include "../base/xchecks.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "xcoroutine.h"
#include "xyieldable.h"

static XrValue m_reset(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrCountdownLatch *latch = xr_value_to_countdown_latch(self);
    XR_DCHECK(latch != NULL, "CountdownLatch.reset: NULL latch");
    int64_t count = nargs >= 1 && XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 0;
    return xr_bool(xr_countdown_latch_reset(latch, count));
}

static XrValue m_done(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrCountdownLatch *latch = xr_value_to_countdown_latch(self);
    XR_DCHECK(latch != NULL, "CountdownLatch.done: NULL latch");
    int64_t count = nargs >= 1 && XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 1;
    return xr_int(xr_countdown_latch_done(latch, count));
}

static XrValue m_try_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_countdown_latch_try_wait(xr_value_to_countdown_latch(self)));
}

static XrCFuncResult ym_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs,
                             XrValue *result) {
    (void) args;
    (void) nargs;
    XrCountdownLatch *latch = xr_value_to_countdown_latch(self);
    XR_DCHECK(latch != NULL, "CountdownLatch.wait: NULL latch");
    XR_DCHECK(result != NULL, "CountdownLatch.wait: NULL result");

    bool ok = false;
    switch (xr_countdown_latch_wait_for_coro(latch, xr_current_coro(isolate), &ok)) {
        case XR_COUNTDOWN_LATCH_WAIT_DONE:
        case XR_COUNTDOWN_LATCH_WAIT_CLOSED:
            *result = xr_bool(ok);
            return XR_CFUNC_DONE;
        case XR_COUNTDOWN_LATCH_WAIT_BLOCKED:
            return XR_CFUNC_BLOCKED;
        case XR_COUNTDOWN_LATCH_WAIT_ERROR:
        default:
            return XR_CFUNC_ERROR;
    }
}

static XrValue m_close(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    xr_countdown_latch_close(xr_value_to_countdown_latch(self));
    return xr_null();
}

static XrValue g_remaining(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(xr_countdown_latch_remaining(xr_value_to_countdown_latch(self)));
}

static XrValue g_is_closed(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_countdown_latch_is_closed(xr_value_to_countdown_latch(self)));
}

static XrValue countdown_latch_construct(XrVMRuntime *isolate, XrValue receiver, XrValue *args,
                                         int nargs) {
    (void) receiver;
    int64_t count = 0;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        count = XR_TO_INT(args[0]);

    XrCountdownLatch *latch = xr_countdown_latch_new(
        xr_isolate_get_runtime_core(isolate), xr_isolate_get_scheduler_runtime(isolate), count);
    if (!latch) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "CountdownLatch allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_countdown_latch(latch);
}

void xr_countdown_latch_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod countdown_latch_methods[] = {
        {"reset", m_reset, 1}, {"done", m_done, 0}, {"tryWait", m_try_wait, 0},
        {"close", m_close, 0}, {NULL, NULL, 0},
    };
    static const XrNativeYieldableMethod countdown_latch_yieldable_methods[] = {
        {"wait", ym_wait, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod countdown_latch_getters[] = {
        {"remaining", g_remaining, 0},
        {"isClosed", g_is_closed, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod countdown_latch_statics[] = {
        {"call", countdown_latch_construct, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo info = {
        .name = "CountdownLatch",
        .gc_type = XR_TCOUNTDOWNLATCH,
        .methods = (XrNativeMethod *) countdown_latch_methods,
        .yieldable_methods = (XrNativeYieldableMethod *) countdown_latch_yieldable_methods,
        .getters = (XrNativeMethod *) countdown_latch_getters,
        .static_methods = (XrNativeMethod *) countdown_latch_statics,
    };
    xr_register_native_type(isolate, &info);
}
