/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xscope_transfer.c - Linked/supervisor scope result transfer implementation.
 */

#include "xscope_transfer.h"

#include "xcoroutine.h"
#include "xdeep_copy.h"
#include "xtask.h"
#include "../base/xglobal_indices.h"
#include "../runtime/class/xenum.h"
#include "../runtime/core/xr_runtime_core.h"
#include "../runtime/object/xarray.h"
#include "../runtime/xisolate_internal.h"

static XrValue scope_child_error(const XrCoroutine *coro) {
    if (!coro)
        return XR_NULL_VAL;
    XrValue err = coro->error;
    if (XR_IS_NULL(err) && coro->task)
        err = coro->task->error;
    return err;
}

static XrValue scope_child_result(const XrCoroutine *coro) {
    if (!coro)
        return XR_NULL_VAL;
    XrValue value = coro->result;
    if (XR_IS_NULL(value) && coro->task)
        value = coro->task->result;
    return value;
}

static bool scope_child_cancelled(const XrCoroutine *coro) {
    if (!coro)
        return true;
    if (xr_coro_flags_has((XrCoroutine *) coro, XR_CORO_FLG_CANCELLED))
        return true;
    if (!coro->task)
        return false;
    uint8_t state = atomic_load_explicit(&coro->task->state, memory_order_acquire);
    return state == XR_TASK_CANCELLED || state == XR_TASK_CANCELLING;
}

static XrValue scope_copy_to_owner(XrCoroutine *coro, XrScopeContext *scope, XrValue value) {
    if (!coro || !scope || !scope->owner || XR_IS_NULL(value))
        return value;
    if (xr_value_needs_copy(value))
        return xr_deep_copy_to_coro_core(coro->core, value, scope->owner);
    return value;
}

static XrValue scope_copy_to_shared(XrCoroutine *coro, XrValue value) {
    if (!coro || !coro->core || XR_IS_NULL(value))
        return value;
    if (xr_value_needs_copy(value))
        return xr_deep_copy_core(coro->core, value, NULL);
    return value;
}

static XrEnumType *scope_task_outcome_type(XrCoroutine *coro) {
    if (!coro || !coro->core)
        return NULL;

    XrValue builtin = xr_runtime_core_builtin(coro->core, XR_GLOBAL_VAR_TASK_OUTCOME);
    XrayIsolate *iso = xr_runtime_core_vm_owner(coro->core);
    if (XR_IS_NULL(builtin) && iso)
        builtin = iso->vm.builtins[XR_GLOBAL_VAR_TASK_OUTCOME];
    if (!XR_IS_PTR(builtin))
        return NULL;
    return XR_TO_ENUM_TYPE(builtin);
}

static XrValue scope_task_outcome_value(XrCoroutine *coro, uint32_t member_index, XrValue payload,
                                        bool has_payload) {
    XrEnumType *enum_type = scope_task_outcome_type(coro);
    if (!enum_type || member_index >= enum_type->member_count)
        return has_payload ? payload : XR_NULL_VAL;

    if (!has_payload) {
        XrEnumValue *member = enum_type->members[member_index].instance;
        return member ? XR_FROM_PTR(member) : XR_NULL_VAL;
    }

    XrValue copied = scope_copy_to_shared(coro, payload);
    XrValue args[1] = {copied};
    XrInstance *inst = xr_enum_adt_construct_core(coro ? coro->core : NULL, NULL, enum_type,
                                                  member_index, args, 1);
    return inst ? XR_FROM_PTR(inst) : XR_NULL_VAL;
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
    } else if (scope->mode == XR_SCOPE_SUPERVISOR) {
        if (scope->outcomes) {
            XrValue outcome = XR_NULL_VAL;
            if (child_failed) {
                outcome = scope_task_outcome_value(coro, 1, err, true);
            } else if (scope_child_cancelled(coro)) {
                outcome = scope_task_outcome_value(coro, 2, XR_NULL_VAL, false);
            } else {
                outcome = scope_task_outcome_value(coro, 0, scope_child_result(coro), true);
            }
            xr_array_push(scope->outcomes, outcome);
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
