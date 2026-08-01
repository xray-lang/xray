/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xscope_transfer.c - Linked scope error transfer implementation.
 */

#include "xscope_transfer.h"

#include "xcoroutine.h"
#include "xtask.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/xshared.h"
#include "../base/xchecks.h"

static XrValue scope_child_error(const XrCoroutine *coro) {
    if (!coro)
        return XR_NULL_VAL;
    XrValue err = coro->error;
    if (XR_IS_NULL(err) && coro->task)
        err = coro->task->error;
    return err;
}

static XrValue scope_observe_for_owner(XrCoroutine *coro, XrScopeContext *scope, XrValue value) {
    if (!coro || !scope || !scope->owner || XR_IS_NULL(value))
        return value;
    if (XR_IS_PTR(value)) {
        XrObjHeader *obj = XR_VALUE_GCPTR(value);
        XR_CHECK(xr_obj_is_publishable_across_executions(obj),
                 "linked scope error requires compiler-planned shared publication");
        if (XR_OBJ_IS_SHARED(obj))
            xr_shared_retain(obj);
    }
    return value;
}

static bool scope_transfer_record_child_completion_locked(XrCoroutine *coro,
                                                          XrScopeContext *scope) {
    if (!coro || !scope || scope->mode != XR_SCOPE_LINKED)
        return false;

    XrValue err = scope_child_error(coro);
    bool child_failed = !XR_IS_NULL(err);

    if (child_failed && XR_IS_NULL(scope->first_error)) {
        scope->first_error = scope_observe_for_owner(coro, scope, err);
        scope->first_error_is_value = coro->error_is_value;
    }

    return child_failed;
}

static const XrScopeTransferOps SCOPE_TRANSFER_OPS = {
    .record_child_completion_locked = scope_transfer_record_child_completion_locked,
};

void xr_scope_transfer_enable_core(XrRuntimeCore *core) {
    xr_runtime_core_set_scope_transfer_ops(core, &SCOPE_TRANSFER_OPS);
}
