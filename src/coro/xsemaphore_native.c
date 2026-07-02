/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemaphore_native.c - VM native Semaphore methods and registration
 */

#include "xsemaphore.h"

#include "../base/xchecks.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "xcoroutine.h"
#include "xyieldable.h"

static XrValue m_release(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    XrSemaphore *sem = xr_value_to_semaphore(self);
    XR_DCHECK(sem != NULL, "Semaphore.release: NULL semaphore");
    int64_t count = nargs >= 1 && XR_IS_INT(args[0]) ? XR_TO_INT(args[0]) : 1;
    return xr_int(xr_semaphore_release(sem, count));
}

static XrValue m_try_acquire(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_semaphore_try_acquire(xr_value_to_semaphore(self)));
}

static XrCFuncResult ym_acquire(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs,
                                XrValue *result) {
    (void) args;
    (void) nargs;
    XrSemaphore *sem = xr_value_to_semaphore(self);
    XR_DCHECK(sem != NULL, "Semaphore.acquire: NULL semaphore");
    XR_DCHECK(result != NULL, "Semaphore.acquire: NULL result");

    bool ok = false;
    switch (xr_semaphore_acquire_for_coro(sem, xr_current_coro(isolate), &ok)) {
        case XR_SEMAPHORE_WAIT_ACQUIRED:
        case XR_SEMAPHORE_WAIT_CLOSED:
            *result = xr_bool(ok);
            return XR_CFUNC_DONE;
        case XR_SEMAPHORE_WAIT_BLOCKED:
            return XR_CFUNC_BLOCKED;
        case XR_SEMAPHORE_WAIT_ERROR:
        default:
            return XR_CFUNC_ERROR;
    }
}

static XrValue m_close(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    xr_semaphore_close(xr_value_to_semaphore(self));
    return xr_null();
}

static XrValue g_available(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int(xr_semaphore_available(xr_value_to_semaphore(self)));
}

static XrValue g_is_closed(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_semaphore_is_closed(xr_value_to_semaphore(self)));
}

static XrValue semaphore_construct(XrVMRuntime *isolate, XrValue receiver, XrValue *args,
                                   int nargs) {
    (void) receiver;
    int64_t permits = 0;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        permits = XR_TO_INT(args[0]);

    XrSemaphore *sem = xr_semaphore_new(xr_isolate_get_runtime_core(isolate),
                                        xr_isolate_get_scheduler_runtime(isolate), permits);
    if (!sem) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "Semaphore allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_semaphore(sem);
}

void xr_semaphore_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod semaphore_methods[] = {
        {"release", m_release, 0},
        {"tryAcquire", m_try_acquire, 0},
        {"close", m_close, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeYieldableMethod semaphore_yieldable_methods[] = {
        {"acquire", ym_acquire, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod semaphore_getters[] = {
        {"available", g_available, 0},
        {"isClosed", g_is_closed, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod semaphore_statics[] = {
        {"call", semaphore_construct, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo info = {
        .name = "Semaphore",
        .gc_type = XR_TSEMAPHORE,
        .methods = (XrNativeMethod *) semaphore_methods,
        .yieldable_methods = (XrNativeYieldableMethod *) semaphore_yieldable_methods,
        .getters = (XrNativeMethod *) semaphore_getters,
        .static_methods = (XrNativeMethod *) semaphore_statics,
    };
    xr_register_native_type(isolate, &info);
}
