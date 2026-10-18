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
#include "../xir/xxir_instance_activation.h"
#include "xr_xir_host_time.h"
#include "../os/os_time.h"
#include "../base/xmalloc.h"

XR_FUNC XrXirTarget xr_xir_host_target(void) {
#if defined(XR_ARCH_X86_64)
    return (XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
#else
    return (XrXirTarget){0};
#endif
}
XR_FUNC XrXirCallStatus xr_xir_host_config_init(XrXirInstanceConfig *config,size_t size) {
    if(!config)return XR_XIR_CALL_BAD_ARGUMENT;
    if(size!=sizeof(*config))return XR_XIR_CALL_BAD_ABI;
    if(!xr_xir_host_target().architecture)return XR_XIR_CALL_UNSUPPORTED;
    XrXirInstanceConfig prepared={0};
    XrXirCallStatus status=xr_xir_instance_config_init(&prepared,sizeof(prepared));
    if(status==XR_XIR_CALL_READY) {
        xr_xir_host_time_provider(&prepared.time);*config=prepared;
    }
    return status;
}

struct XrXirHostCall {
    XrXirInstance *instance;
    XrXirHostInstance *owner;
    XrXirInstanceResult observed;
    bool busy, consumed;
};
struct XrXirHostInstance {
    XrXirInstance *instance;
    XrXirHostCall call;
    bool active, busy, detached;
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
XR_FUNC XrXirCallStatus xr_xir_host_instance_new(XrXirProgram *program,
    const XrXirInstanceConfig *input, XrXirHostInstance **output) {
    if (!program || !input || !output || *output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (input->abi_version != XR_XIR_CALL_ABI_VERSION || input->struct_size != sizeof(*input) ||
        !xr_xir_output_provider_valid(&input->output)) return XR_XIR_CALL_BAD_ABI;
    if (input->metadata_limit < sizeof(XrXirHostInstance)) return XR_XIR_CALL_LIMIT;
    XrXirHostInstance *owner = xr_calloc(1, sizeof(*owner));
    if (!owner) return XR_XIR_CALL_OOM;
    XrXirInstanceConfig config = *input;
    config.metadata_limit -= sizeof(*owner);
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &owner->instance);
    if (status != XR_XIR_CALL_READY) { xr_free(owner); return status; }
    *output = owner;
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus host_instance_begin(XrXirHostInstance *owner, uint32_t entry,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count, XrXirHostCall **output) {
    if (!owner || !output || *output || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (owner->busy || owner->active) return XR_XIR_CALL_BUSY;
    owner->busy = true;
    XrXirCallStatus status = function ? xr_xir_instance_start_function(owner->instance, function, arguments, count) :
        xr_xir_instance_start(owner->instance, entry, arguments, count);
    owner->busy = false;
    if (status != XR_XIR_CALL_READY) return status;
    owner->call = (XrXirHostCall){.instance = owner->instance, .owner = owner};
    owner->active = true;
    *output = &owner->call;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_host_instance_begin(XrXirHostInstance *owner, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirHostCall **output) {
    return host_instance_begin(owner, entry, NULL, arguments, count, output);
}
XR_FUNC XrXirCallStatus xr_xir_host_instance_begin_function(XrXirHostInstance *owner,
    const XrXirValue *function, const XrXirValue *arguments, uint32_t count, XrXirHostCall **output) {
    if (!function) return XR_XIR_CALL_BAD_ARGUMENT;
    return host_instance_begin(owner, 0, function, arguments, count, output);
}
XR_FUNC XrXirCallStatus xr_xir_host_instance_drop(XrXirHostInstance *owner) {
    if (!owner) return XR_XIR_CALL_READY;
    if (owner->busy || owner->active) return XR_XIR_CALL_BUSY;
    owner->busy = true;
    XrXirCallStatus status = xr_xir_instance_free(owner->instance);
    if (status == XR_XIR_CALL_BUSY) { owner->busy = false; return status; }
    xr_free(owner);
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_host_call_begin(const XrXirHostExecutionRequest *request,
    XrXirHostCall **output) {
    if (!request || !request->program || !request->config ||
        (request->argument_count && !request->arguments) || !output || *output)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirHostInstance *owner = NULL;
    XrXirCallStatus status = xr_xir_host_instance_new(request->program, request->config, &owner);
    if (status == XR_XIR_CALL_READY)
        status = xr_xir_host_instance_begin(owner, request->entry, request->arguments, request->argument_count, output);
    if (status != XR_XIR_CALL_READY) {
        XrXirCallStatus cleanup = xr_xir_host_instance_drop(owner);
        return cleanup == XR_XIR_CALL_READY ? status : cleanup;
    }
    owner->detached = true;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirInstanceResult xr_xir_host_call_step_bounded(XrXirHostCall *call, uint64_t quantum) {
    if (!call || !quantum) return host_status(XR_XIR_CALL_BAD_ARGUMENT);
    if (call->busy) return host_status(XR_XIR_CALL_BUSY);
    if (call->consumed) return host_status(XR_XIR_CALL_BAD_STATE);
    call->busy = true;
    call->observed = xr_xir_instance_poll_bounded(call->instance, quantum);
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
XR_FUNC XrXirCallStatus xr_xir_host_call_wait_request(const XrXirHostCall *call, uint64_t epoch,
    uint64_t wake, XrXirWaitRequest *output) {
    if (!call || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->busy) return XR_XIR_CALL_BUSY;
    if (call->consumed || call->observed.outcome.status != XR_XIR_CALL_SUSPENDED ||
        call->observed.epoch != epoch || call->observed.outcome.wake != wake) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_instance_wait_request(call->instance, epoch, wake, output);
}
XR_FUNC XrXirCallStatus xr_xir_host_call_request_cancel(XrXirHostCall *call) {
    if (!call) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->busy) return XR_XIR_CALL_BUSY;
    if (call->consumed || (call->observed.outcome.status != XR_XIR_CALL_READY &&
        call->observed.outcome.status != XR_XIR_CALL_SUSPENDED)) return XR_XIR_CALL_BAD_STATE;
    call->busy = true;
    XrXirCallStatus status = xr_xir_instance_cancel_current(call->instance);
    if (status == XR_XIR_CALL_CANCEL_REQUESTED) call->observed = host_status(XR_XIR_CALL_READY);
    call->busy = false;
    return status;
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
    XrXirHostInstance *owner = call->owner;
    owner->busy = true;
    XrXirCallStatus status = owner->detached ? xr_xir_instance_free(call->instance) :
        xr_xir_instance_close_activation(call->instance);
    owner->busy = false;
    if (status == XR_XIR_CALL_BUSY) { call->busy = false; return status; }
    owner->active = false;
    if (owner->detached) xr_free(owner);
    else owner->call = (XrXirHostCall){0};
    return status;
}
static XrXirCallStatus host_complete(XrXirHostCall *call, XrXirCallResult *output) {
    XrXirCallStatus status = XR_XIR_CALL_READY;
    XrXirInstanceResult observed = xr_xir_host_call_step_bounded(call, 256);
    while (observed.outcome.status == XR_XIR_CALL_READY ||
        observed.outcome.status == XR_XIR_CALL_SUSPENDED) {
        if (observed.outcome.status == XR_XIR_CALL_SUSPENDED) {
            XrXirWaitRequest wait_request = {0};
            XrXirHostWait wait = {0};
            status = xr_xir_host_call_wait_request(call, observed.epoch, observed.outcome.wake, &wait_request);
            if (status==XR_XIR_CALL_READY && wait_request.kind==XR_XIR_WAIT_CHANNEL) {
                /* The activation arbitrates its owned peer intent and deadline.
                 * Sleeping only paces observation and never authorizes a resume. */
                xr_time_sleep_ms(1);
                observed=xr_xir_host_call_step_bounded(call,256);
                continue;
            }
            XrXirHostWaitStatus waiting = status == XR_XIR_CALL_READY ?
                xr_xir_host_wait_begin(&wait, &wait_request) : XR_XIR_HOST_WAIT_BAD_ARGUMENT;
            while (waiting == XR_XIR_HOST_WAIT_PENDING)
                waiting = xr_xir_host_wait_poll(&wait, UINT64_MAX);
            if (waiting != XR_XIR_HOST_WAIT_DUE) {
                /* The host cannot honor the wait: drain the activation and report the host failure. */
                if (xr_xir_host_call_request_cancel(call) == XR_XIR_CALL_CANCEL_REQUESTED) {
                    do observed = xr_xir_host_call_step_bounded(call, 256);
                    while (observed.outcome.status == XR_XIR_CALL_READY);
                }
                status = XR_XIR_CALL_HOST_ERROR;
                break;
            }
            status = xr_xir_host_call_resume(call, observed.epoch, observed.outcome.wake);
            if (status != XR_XIR_CALL_READY) break;
        }
        observed = xr_xir_host_call_step_bounded(call, 256);
    }
    if (status == XR_XIR_CALL_HOST_ERROR) {
        XrXirCallStatus cleanup_failed = xr_xir_host_call_drop(call);
        return cleanup_failed != XR_XIR_CALL_READY ? cleanup_failed : status;
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
XR_FUNC XrXirCallStatus xr_xir_host_execute(const XrXirHostExecutionRequest *request,
    XrXirCallResult *output) {
    if (!xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirHostCall *call = NULL;
    XrXirCallStatus status = xr_xir_host_call_begin(request, &call);
    return status == XR_XIR_CALL_READY ? host_complete(call, output) : status;
}
XR_FUNC XrXirCallStatus xr_xir_host_instance_execute(XrXirHostInstance *owner, uint32_t entry,
    const XrXirValue *arguments, uint32_t count, XrXirCallResult *output) {
    if (!xr_xir_call_result_empty(output)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirHostCall *call = NULL;
    XrXirCallStatus status = xr_xir_host_instance_begin(owner, entry, arguments, count, &call);
    return status == XR_XIR_CALL_READY ? host_complete(call, output) : status;
}
