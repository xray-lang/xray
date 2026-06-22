/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xscope_transfer.c - Linked/supervisor scope error transfer implementation.
 */

#include "xscope_transfer.h"

#include "xaot_coro.h"
#include "xcoroutine.h"
#include "xdeep_copy.h"
#include "xtask.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/object/xarray.h"
#include "../runtime/object/xexception.h"
#include "../runtime/object/xstring.h"
#include <string.h>

static XrValue scope_child_error(const XrCoroutine *coro) {
    if (!coro)
        return XR_NULL_VAL;
    XrValue err = coro->error;
    if (XR_IS_NULL(err) && coro->task)
        err = coro->task->error;
    return err;
}

static bool scope_transfer_record_child_error_locked(XrCoroutine *coro, XrScopeContext *scope) {
    if (!coro || !scope || scope->mode == XR_SCOPE_WAIT)
        return false;

    XrValue err = scope_child_error(coro);
    if (XR_IS_NULL(err))
        return false;

    if (scope->mode == XR_SCOPE_LINKED) {
        if (XR_IS_NULL(scope->first_error)) {
            if (scope->owner && xr_value_needs_copy(err))
                err = xr_deep_copy_to_coro_core(coro->core, err, scope->owner);
            scope->first_error = err;
            scope->first_error_is_value = coro->error_is_value;
        }
    } else if (scope->mode == XR_SCOPE_SUPERVISOR) {
        if (scope->errors) {
            XrValue msg = err;
            XrayIsolate *iso = coro->isolate;
            if (iso && xr_value_is_exception(iso, err)) {
                const char *m = xr_exception_get_message(iso, err);
                if (!m)
                    m = "";
                XrString *s = xr_string_intern_core(coro->core, m, strlen(m), 0);
                msg = s ? xr_string_value(s) : XR_NULL_VAL;
            }
            xr_array_push(scope->errors, msg);
        }
    }

    return true;
}

static const XrScopeTransferOps SCOPE_TRANSFER_OPS = {
    .record_child_error_locked = scope_transfer_record_child_error_locked,
};

void xr_scope_transfer_enable_core(XrRuntimeCore *core) {
    xr_runtime_core_set_scope_transfer_ops(core, &SCOPE_TRANSFER_OPS);
}

void xr_aot_runtime_enable_transfer(XrAotRuntime *runtime) {
    xr_scope_transfer_enable_core(xr_aot_runtime_core(runtime));
}
