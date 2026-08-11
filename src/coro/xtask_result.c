/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtask_result.c - Verified Task result owner/capability handoff.
 */

#include "xtask.h"
#include "../runtime/xshared.h"
#include "../base/xchecks.h"

static inline void task_result_lock_acquire(_Atomic bool *lock) {
    while (atomic_exchange_explicit(lock, true, memory_order_acquire)) {
    }
}

static inline void task_result_lock_release(_Atomic bool *lock) {
    atomic_store_explicit(lock, false, memory_order_release);
}

XrValue xr_task_consume_result(struct XrRuntimeCore *core, XrTask *task,
                               struct XrCoroutine *dst_coro) {
    (void) core;
    (void) dst_coro;
    /* XrValue is 16 bytes, so serialize the take/retain decision against
     * concurrent awaiters. Publication of COMPLETED is release/acquire. */
    task_result_lock_acquire(&task->await_lock);
    XrValue result = task->result;
    if (task->result_owner == XR_TASK_PAYLOAD_TRANSFER &&
        xr_task_value_is_transfer_payload(result)) {
        if (atomic_load_explicit(&task->payload_taken, memory_order_relaxed) != 0) {
            task_result_lock_release(&task->await_lock);
            return xr_null();
        }
        atomic_store_explicit(&task->payload_taken, 1, memory_order_relaxed);
        task->result = xr_null();
        task->result_owner = XR_TASK_PAYLOAD_NONE;
    } else if (atomic_load_explicit(&task->payload_taken, memory_order_relaxed) != 0) {
        result = xr_null();
    } else if (task->result_owner == XR_TASK_PAYLOAD_SHARED && XR_IS_PTR(result)) {
        if (XR_IS_STRING(result)) {
            XR_CHECK(xr_value_runtime_string_is_shared(result),
                     "Task shared string result owner mismatch");
            XR_CHECK(xr_runtime_object_header_retain(
                         xr_value_runtime_object_header(result)) ==
                         XR_RUNTIME_ABI_OK,
                     "Task shared string retain mismatch");
        } else {
            XrObjHeader *obj = XR_VALUE_GCPTR(result);
            XR_CHECK(obj && XR_OBJ_IS_SHARED(obj),
                     "Task shared result owner mismatch");
            xr_obj_dup(obj);
        }
    } else if (XR_IS_PTR(result)) {
        if (XR_IS_STRING(result)) {
            XR_CHECK(xr_value_runtime_string_is_shared(result) ||
                         xr_value_runtime_string_is_transferable(result),
                     "Task string result bypassed verified storage publication");
        } else {
            XrObjHeader *obj = XR_VALUE_GCPTR(result);
            XR_CHECK(obj && (XR_OBJ_IS_SHARED(obj) || XR_OBJ_IS_TRANSFER(obj)),
                     "Task result bypassed verified storage publication");
        }
    }
    task_result_lock_release(&task->await_lock);
    return result;
}

XrValue xr_task_observe_error(struct XrRuntimeCore *core, XrTask *task,
                              struct XrCoroutine *dst_coro) {
    (void) core;
    (void) dst_coro;
    if (!task)
        return xr_null();
    task_result_lock_acquire(&task->await_lock);
    XrValue error = task->error;
    if (task->error_owner == XR_TASK_PAYLOAD_TRANSFER && xr_task_value_is_transfer_payload(error)) {
        if (atomic_load_explicit(&task->payload_taken, memory_order_relaxed) != 0) {
            task_result_lock_release(&task->await_lock);
            return xr_null();
        }
        atomic_store_explicit(&task->payload_taken, 1, memory_order_relaxed);
        task->error = xr_null();
        task->error_owner = XR_TASK_PAYLOAD_NONE;
    } else if (atomic_load_explicit(&task->payload_taken, memory_order_relaxed) != 0) {
        error = xr_null();
    } else if (task->error_owner == XR_TASK_PAYLOAD_SHARED && XR_IS_PTR(error)) {
        if (XR_IS_STRING(error)) {
            XR_CHECK(xr_value_runtime_string_is_shared(error),
                     "Task shared string error owner mismatch");
            XR_CHECK(xr_runtime_object_header_retain(
                         xr_value_runtime_object_header(error)) ==
                         XR_RUNTIME_ABI_OK,
                     "Task shared string retain mismatch");
        } else {
            XrObjHeader *obj = XR_VALUE_GCPTR(error);
            XR_CHECK(obj && XR_OBJ_IS_SHARED(obj),
                     "Task shared error owner mismatch");
            xr_obj_dup(obj);
        }
    } else if (XR_IS_PTR(error)) {
        if (XR_IS_STRING(error)) {
            XR_CHECK(xr_value_runtime_string_is_shared(error) ||
                         xr_value_runtime_string_is_transferable(error),
                     "Task string error bypassed verified storage publication");
        } else {
            XrObjHeader *obj = XR_VALUE_GCPTR(error);
            XR_CHECK(obj && (XR_OBJ_IS_SHARED(obj) || XR_OBJ_IS_TRANSFER(obj)),
                     "Task error bypassed verified storage publication");
        }
    }
    task_result_lock_release(&task->await_lock);
    return error;
}
