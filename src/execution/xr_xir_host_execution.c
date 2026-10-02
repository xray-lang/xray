/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_host_execution.c - One lifetime for typed host program invocation
 *
 * KEY CONCEPT:
 *   Poll outcomes borrow until copied before physical instance destruction.
 *   Suspension tokens are resumed exactly and are never interpreted as clocks.
 */
#include "xr_xir_host_execution.h"
#include "../os/os_time.h"

static XrXirCallStatus copy_status(XrXirValueStatus status) {
    switch (status) {
    case XR_XIR_VALUE_OK: return XR_XIR_CALL_READY;
    case XR_XIR_VALUE_OOM: return XR_XIR_CALL_OOM;
    case XR_XIR_VALUE_LIMIT:
    case XR_XIR_VALUE_REFCOUNT_LIMIT: return XR_XIR_CALL_LIMIT;
    default: return XR_XIR_CALL_BAD_ARGUMENT;
    }
}
XR_FUNC XrXirCallStatus xr_xir_host_execute(const XrXirHostExecutionRequest *request,
    XrXirCallResult *output) {
    if (!request || !request->program || !request->config ||
        (request->argument_count && !request->arguments) || !xr_xir_call_result_empty(output))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance = NULL;
    XrXirCallStatus status = xr_xir_instance_new(request->program, request->config, &instance);
    if (status != XR_XIR_CALL_READY) return status;
    status = xr_xir_instance_start(instance, request->entry,
        request->arguments, request->argument_count);
    XrXirCallResult owned = {0};
    if (status == XR_XIR_CALL_READY) {
        XrXirInstanceResult borrowed = xr_xir_instance_poll(instance);
        while (borrowed.outcome.status == XR_XIR_CALL_SUSPENDED) {
            xr_time_sleep_ns(0);
            status = xr_xir_instance_resume(instance, borrowed.epoch, borrowed.outcome.wake);
            if (status != XR_XIR_CALL_READY) break;
            borrowed = xr_xir_instance_poll(instance);
        }
        if (status == XR_XIR_CALL_READY) {
            status = copy_status(xr_xir_call_result_copy(&borrowed.outcome, &owned));
            if (status == XR_XIR_CALL_READY) status = borrowed.outcome.status;
        }
    }
    XrXirCallStatus cleanup = xr_xir_instance_free(instance);
    if (cleanup != XR_XIR_CALL_READY) {
        xr_xir_call_result_drop(&owned);
        return cleanup;
    }
    if (!xr_xir_call_result_empty(&owned)) xr_xir_call_result_move(&owned, output);
    return status;
}
