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
#include "xdeep_copy.h"
#include "xtask.h"
#include "../runtime/core/xr_runtime_core.h"

static XrValue scope_child_error(const XrCoroutine *coro) {
    if (!coro)
        return XR_NULL_VAL;
    XrValue err = coro->error;
    if (XR_IS_NULL(err) && coro->task)
        err = coro->task->error;
    return err;
}

static XrValue scope_copy_to_owner(XrCoroutine *coro, XrScopeContext *scope, XrValue value) {
    if (!coro || !scope || !scope->owner || XR_IS_NULL(value))
        return value;
    if (xr_value_needs_copy(value))
        return xr_deep_copy_to_coro_core(coro->core, value, scope->owner);
    return value;
}

static bool scope_transfer_record_child_completion_locked(XrCoroutine *coro,
                                                          XrScopeContext *scope) {
    if (!coro || !scope || scope->mode == XR_SCOPE_WAIT)
        return false;

    XrValue err = scope_child_error(coro);
    bool child_failed = !XR_IS_NULL(err);

    if (scope->mode == XR_SCOPE_LINKED) {
        if (child_failed && XR_IS_NULL(scope->first_error)) {
            scope->first_error = scope_copy_to_owner(coro, scope, err);
            scope->first_error_is_value = coro->error_is_value;
        }
    }

    return child_failed;
}

static const XrScopeTransferOps SCOPE_TRANSFER_OPS = {
    .record_child_completion_locked = scope_transfer_record_child_completion_locked,
};

void xr_scope_transfer_enable_core(XrRuntimeCore *core) {
    xr_runtime_core_set_scope_transfer_ops(core, &SCOPE_TRANSFER_OPS);
}
