/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompile_request.c - Request-local safe-point cancellation and deadlines
 */
#include "xcompile_request.h"
#include "../os/os_time.h"

XR_FUNC XrCompileResourceStatus xr_compile_request_control_init(
    const XrCompileRequestOptions *options, XrCompileRequestControl *output) {
    if (!options || !output || output->initialized || output->polling ||
        (!options->cancel_poll && options->cancel_context) ||
        (options->deadline_mode != XR_COMPILE_DEADLINE_NONE &&
         options->deadline_mode != XR_COMPILE_DEADLINE_AFTER) ||
        (options->deadline_mode == XR_COMPILE_DEADLINE_NONE && options->timeout_ns))
        return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileRequestControl control = {0};
    control.cancel_poll = options->cancel_poll;
    control.cancel_context = options->cancel_context;
    control.has_deadline = options->deadline_mode == XR_COMPILE_DEADLINE_AFTER;
    if (control.has_deadline) {
        if (!options->timeout_ns) control.status = XR_COMPILE_RESOURCE_DEADLINE;
        else {
            uint64_t now = xr_time_monotonic_ns();
            if (!now) return XR_COMPILE_RESOURCE_CLOCK_FAILURE;
            if (options->timeout_ns > UINT64_MAX - now) return XR_COMPILE_RESOURCE_OVERFLOW;
            control.deadline_ns = now + options->timeout_ns;
        }
    }
    control.initialized = true;
    *output = control;
    return XR_COMPILE_RESOURCE_OK;
}

XR_FUNC XrCompileResourceStatus xr_compile_request_control_poll(XrCompileRequestControl *control) {
    if (!control || !control->initialized || control->polling) return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if (control->status != XR_COMPILE_RESOURCE_OK) return control->status;
    if (control->has_deadline) {
        uint64_t now = xr_time_monotonic_ns();
        if (!now) control->status = XR_COMPILE_RESOURCE_CLOCK_FAILURE;
        else if (now >= control->deadline_ns) control->status = XR_COMPILE_RESOURCE_DEADLINE;
        if (control->status != XR_COMPILE_RESOURCE_OK) return control->status;
    }
    control->polling = true;
    bool cancelled = control->cancel_poll && control->cancel_poll(control->cancel_context);
    control->polling = false;
    if (cancelled) control->status = XR_COMPILE_RESOURCE_CANCELLED;
    return control->status;
}
