/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresult_group_native.c - VM native ResultGroup methods and registration
 */

#include "xresult_group.h"

#include "../base/xchecks.h"
#include "../runtime/object/xpanic_info.h"
#include "../runtime/object/xnative_type.h"
#include "../runtime/object/xtuple.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "xcoroutine.h"
#include "xyieldable.h"

static uint32_t sanitize_batch_size(int64_t value) {
    if (value <= 0)
        return XR_RESULT_GROUP_DEFAULT_BATCH;
    if (value > XR_RESULT_GROUP_MAX_BATCH)
        return XR_RESULT_GROUP_MAX_BATCH;
    return (uint32_t) value;
}

static XrValue m_add(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) nargs;
    XrResultGroup *g = xr_value_to_result_group(self);
    XR_DCHECK(g != NULL, "ResultGroup.add: NULL group");
    XR_DCHECK(nargs >= 1, "ResultGroup.add: missing value");
    if (!XR_IS_INT(args[0]))
        return xr_bool(false);
    return xr_bool(xr_result_group_add(g, XR_TO_INT(args[0])));
}

static XrValue m_flush(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    xr_result_group_flush(xr_value_to_result_group(self));
    return xr_null();
}

static XrValue m_try_recv(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) args;
    (void) nargs;
    XrResultGroup *g = xr_value_to_result_group(self);
    XR_DCHECK(g != NULL, "ResultGroup.tryRecv: NULL group");
    int64_t value = 0;
    bool ok = xr_result_group_try_recv(g, &value);
    XrTuple *tuple = xr_tuple_new(xr_current_coro(isolate), 2);
    if (!tuple)
        return xr_null();
    xr_tuple_set(tuple, 0, ok ? xr_int(value) : xr_null());
    xr_tuple_set(tuple, 1, xr_bool(ok));
    return xr_value_from_tuple(tuple);
}

static XrCFuncResult ym_recv(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs,
                             XrValue *result) {
    (void) args;
    (void) nargs;
    XrResultGroup *g = xr_value_to_result_group(self);
    XR_DCHECK(g != NULL, "ResultGroup.recv: NULL group");
    XR_DCHECK(result != NULL, "ResultGroup.recv: NULL result");

    switch (xr_result_group_recv_for_coro(g, xr_current_coro(isolate), result)) {
        case XR_RESULT_GROUP_RECV_DONE:
            return XR_CFUNC_DONE;
        case XR_RESULT_GROUP_RECV_BLOCKED:
            return XR_CFUNC_BLOCKED;
        case XR_RESULT_GROUP_RECV_ERROR:
        default:
            return XR_CFUNC_ERROR;
    }
}

static XrValue m_close(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    xr_result_group_close(xr_value_to_result_group(self));
    return xr_null();
}

static XrValue g_length(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) xr_result_group_length(xr_value_to_result_group(self)));
}

static XrValue g_pending_count(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_int((int64_t) xr_result_group_pending_count(xr_value_to_result_group(self)));
}

static XrValue g_batch_size(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    XrResultGroup *g = xr_value_to_result_group(self);
    return xr_int(g ? (int64_t) g->batch_size : 0);
}

static XrValue g_is_closed(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs) {
    (void) isolate;
    (void) args;
    (void) nargs;
    return xr_bool(xr_result_group_is_closed(xr_value_to_result_group(self)));
}

static XrValue result_group_construct(XrVMRuntime *isolate, XrValue receiver, XrValue *args,
                                      int nargs) {
    (void) receiver;
    uint32_t batch_size = XR_RESULT_GROUP_DEFAULT_BATCH;
    if (nargs >= 1 && XR_IS_INT(args[0]))
        batch_size = sanitize_batch_size(XR_TO_INT(args[0]));

    XrResultGroup *g = xr_result_group_new(xr_isolate_get_runtime_core(isolate),
                                           xr_isolate_get_scheduler_runtime(isolate), batch_size);
    if (!g) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "ResultGroup allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_result_group(g);
}

void xr_result_group_register_native_type(XrVMRuntime *isolate) {
    static const XrNativeMethod result_group_methods[] = {
        {"add", m_add, 1},     {"flush", m_flush, 0}, {"tryRecv", m_try_recv, 0},
        {"close", m_close, 0}, {NULL, NULL, 0},
    };
    static const XrNativeYieldableMethod result_group_yieldable_methods[] = {
        {"recv", ym_recv, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod result_group_getters[] = {
        {"length", g_length, 0},
        {"pendingCount", g_pending_count, 0},
        {"batchSize", g_batch_size, 0},
        {"isClosed", g_is_closed, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeMethod result_group_statics[] = {
        {"call", result_group_construct, 0},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo info = {
        .name = "ResultGroup",
        .gc_type = XR_TRESULTGROUP,
        .methods = (XrNativeMethod *) result_group_methods,
        .yieldable_methods = (XrNativeYieldableMethod *) result_group_yieldable_methods,
        .getters = (XrNativeMethod *) result_group_getters,
        .static_methods = (XrNativeMethod *) result_group_statics,
    };
    xr_register_native_type(isolate, &info);
}
