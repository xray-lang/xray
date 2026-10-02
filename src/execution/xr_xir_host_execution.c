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
#include "../base/xmalloc.h"

struct XrXirHostCall {
    XrXirInstance *instance;
    XrXirInstanceResult observed;
    bool busy, consumed;
};

static XrXirInstanceResult host_status(XrXirCallStatus status) {
    XrXirInstanceResult result = {0};
    result.outcome.status = status;
    return result;
}

static XrXirCallStatus copy_status(XrXirValueStatus status) {
    switch (status) {
    case XR_XIR_VALUE_OK: return XR_XIR_CALL_READY;
    case XR_XIR_VALUE_OOM: return XR_XIR_CALL_OOM;
    case XR_XIR_VALUE_LIMIT:
    case XR_XIR_VALUE_REFCOUNT_LIMIT: return XR_XIR_CALL_LIMIT;
    default: return XR_XIR_CALL_BAD_ARGUMENT;
    }
}
XR_FUNC XrXirCallStatus xr_xir_host_call_begin(const XrXirHostExecutionRequest *request,
    XrXirHostCall **output) {
    if (!request || !request->program || !request->config ||
        (request->argument_count && !request->arguments) || !output || *output)
        return XR_XIR_CALL_BAD_ARGUMENT;
    if (request->config->abi_version != XR_XIR_CALL_ABI_VERSION ||
        request->config->struct_size != sizeof(*request->config) ||
        !xr_xir_output_provider_valid(&request->config->output)) return XR_XIR_CALL_BAD_ABI;
    if (request->config->metadata_limit < sizeof(XrXirHostCall)) return XR_XIR_CALL_LIMIT;
    XrXirHostCall *call = xr_calloc(1, sizeof(*call));
    if (!call) return XR_XIR_CALL_OOM;
    XrXirInstanceConfig config = *request->config;
    config.metadata_limit -= sizeof(*call);
    XrXirInstance *instance = NULL;
    XrXirCallStatus status = xr_xir_instance_new(request->program, &config, &instance);
    if (status == XR_XIR_CALL_READY)
        status = xr_xir_instance_start(instance, request->entry, request->arguments, request->argument_count);
    if (status != XR_XIR_CALL_READY) {
        XrXirCallStatus cleanup = xr_xir_instance_free(instance);
        xr_free(call);
        return cleanup == XR_XIR_CALL_READY ? status : cleanup;
    }
    call->instance = instance;
    *output = call;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirInstanceResult xr_xir_host_call_step(XrXirHostCall *call) {
    if (!call) return host_status(XR_XIR_CALL_BAD_ARGUMENT);
    if (call->busy) return host_status(XR_XIR_CALL_BUSY);
    if (call->consumed) return host_status(XR_XIR_CALL_BAD_STATE);
    call->busy = true;
    call->observed = xr_xir_instance_poll(call->instance);
    call->busy = false;
    return call->observed;
}
XR_FUNC XrXirCallStatus xr_xir_host_call_resume(XrXirHostCall *call, uint64_t epoch, uint64_t wake) {
    if (!call) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->busy) return XR_XIR_CALL_BUSY;
    if (call->consumed || call->observed.outcome.status != XR_XIR_CALL_SUSPENDED)
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = xr_xir_instance_resume(call->instance, epoch, wake);
    if (status == XR_XIR_CALL_READY) call->observed = host_status(status);
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_host_call_cancel(XrXirHostCall *call) {
    if (!call) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->busy) return XR_XIR_CALL_BUSY;
    if (call->consumed || (call->observed.outcome.status != XR_XIR_CALL_READY &&
        call->observed.outcome.status != XR_XIR_CALL_SUSPENDED)) return XR_XIR_CALL_BAD_STATE;
    call->busy = true;
    XrXirCallStatus status = xr_xir_instance_stop(call->instance);
    call->observed = xr_xir_instance_poll(call->instance);
    call->busy = false;
    return status == XR_XIR_CALL_READY ? call->observed.outcome.status : status;
}
XR_FUNC XrXirCallStatus xr_xir_host_call_take(XrXirHostCall *call, XrXirCallResult *output) {
    if (!call || !xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->busy) return XR_XIR_CALL_BUSY;
    if (call->consumed || call->observed.outcome.status == XR_XIR_CALL_READY ||
        call->observed.outcome.status == XR_XIR_CALL_SUSPENDED) return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status = copy_status(xr_xir_call_result_copy(&call->observed.outcome, output));
    if (status != XR_XIR_CALL_READY) return status;
    call->consumed = true;
    return output->status;
}
XR_FUNC XrXirCallStatus xr_xir_host_call_drop(XrXirHostCall *call) {
    if (!call) return XR_XIR_CALL_READY;
    if (call->busy) return XR_XIR_CALL_BUSY;
    call->busy = true;
    XrXirCallStatus status = xr_xir_instance_free(call->instance);
    if (status == XR_XIR_CALL_BUSY) { call->busy = false; return status; }
    xr_free(call);
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_host_execute(const XrXirHostExecutionRequest *request,
    XrXirCallResult *output) {
    if (!xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirHostCall *call = NULL;
    XrXirCallStatus status = xr_xir_host_call_begin(request, &call);
    if (status != XR_XIR_CALL_READY) return status;
    XrXirInstanceResult observed = xr_xir_host_call_step(call);
    while (observed.outcome.status == XR_XIR_CALL_SUSPENDED) {
        xr_time_sleep_ns(0);
        status = xr_xir_host_call_resume(call, observed.epoch, observed.outcome.wake);
        if (status != XR_XIR_CALL_READY) break;
        observed = xr_xir_host_call_step(call);
    }
    XrXirCallResult owned = {0};
    if (status == XR_XIR_CALL_READY) status = xr_xir_host_call_take(call, &owned);
    XrXirCallStatus cleanup = xr_xir_host_call_drop(call);
    if (cleanup != XR_XIR_CALL_READY) {
        xr_xir_call_result_drop(&owned);
        return cleanup;
    }
    if (!xr_xir_call_result_empty(&owned)) xr_xir_call_result_move(&owned, output);
    return status;
}
