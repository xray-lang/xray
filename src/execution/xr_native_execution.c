/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_native_execution.c - Native entry execution with an admitted instance lease
 */

#include "xr_native_execution.h"
#include "../base/xmalloc.h"

typedef struct XrNativeInstanceState {
    void *payload;
    XrBackendNativeDrop drop;
    XrBackendNativeDrop abort;
    XrBackendNativeOutcome failure;
} XrNativeInstanceState;

struct XrBackendExecution {
    XrExecutionLease lease;
    XrNativeInstanceState *state;
    XrBackendNativeModuleStep module_step;
    XrBackendNativeStep step;
    XrBackendNativeCancel cancel;
    XrBackendNativeDrop drop;
    void *frame;
    XrBackendExecutionOutcome last;
    XrExecutionInitializationStep initializer;
    bool initializing;
    bool finished;
};

static void native_state_destroy(void *opaque) {
    XrNativeInstanceState *state = opaque;
    state->drop(state->payload);
    xr_free(state->payload);
    xr_free(state);
}

static int native_output_write(void *opaque, uint32_t requirement, uint32_t operation,
                               const uint8_t *bytes, size_t size) {
    XrBackendExecution *execution = opaque;
    return xr_execution_lease_provider_output_write(&execution->lease, requirement, operation,
                                                    bytes, size) != XR_EXECUTION_PROVIDER_CALL_OK;
}

static int native_typed_call(void *opaque, uint32_t requirement, uint32_t operation,
                              const XrProviderValuePack *arguments, XrProviderValuePack *result) {
    XrBackendExecution *execution = opaque;
    XrExecutionProviderCallResult status = xr_execution_lease_provider_call_typed(
        &execution->lease, requirement, operation, arguments, result);
    return status == XR_EXECUTION_PROVIDER_CALL_OK ? XR_BACKEND_NATIVE_PROVIDER_OK :
           status == XR_EXECUTION_PROVIDER_CALL_OUT_OF_MEMORY ? XR_BACKEND_NATIVE_PROVIDER_RESOURCE_LIMIT :
           XR_BACKEND_NATIVE_PROVIDER_FAILED;
}

bool xr_backend_execution_create(XrInstance *instance, const XrBackendNativeDescriptor *descriptor,
                                 XrBackendExecution **execution_out) {
    if (execution_out)
        *execution_out = NULL;
    if (!instance || !descriptor || !execution_out ||
        descriptor->schema_version != XR_BACKEND_NATIVE_DESCRIPTOR_SCHEMA_VERSION ||
        descriptor->reserved32 != 0u || descriptor->frame_size == 0u || !descriptor->state_layout ||
        descriptor->state_size == 0u || !descriptor->state_initialize || !descriptor->state_drop ||
        !descriptor->initialization_abort || !descriptor->module_step || !descriptor->initialize ||
        !descriptor->step || !descriptor->cancel || !descriptor->drop)
        return false;
    XrBackendExecution *execution = xr_calloc(1u, sizeof(*execution));
    if (!execution)
        return false;
    if (xr_execution_instance_acquire(instance, &execution->lease) != XR_EXECUTION_OK) {
        xr_free(execution);
        return false;
    }
    XrExecutionId execution_id = xr_execution_instance_id(execution->lease.instance);
    if (!xr_fingerprint_equal(descriptor->execution_id, execution_id)) {
        (void) xr_execution_lease_release(&execution->lease);
        xr_free(execution);
        return false;
    }
    execution->frame = xr_calloc(1u, descriptor->frame_size);
    if (!execution->frame) {
        (void) xr_execution_lease_release(&execution->lease);
        xr_free(execution);
        return false;
    }
    void *bound_state = NULL;
    XrExecutionStateBindingResult binding = xr_execution_lease_bind_state(
        &execution->lease, descriptor->state_layout, NULL, NULL, &bound_state);
    if (binding == XR_EXECUTION_STATE_EMPTY) {
        XrNativeInstanceState *candidate = xr_calloc(1u, sizeof(*candidate));
        if (candidate) {
            candidate->payload = xr_calloc(1u, descriptor->state_size);
            if (candidate->payload) {
                candidate->drop = descriptor->state_drop;
                candidate->abort = descriptor->initialization_abort;
                candidate->failure.kind = XR_BACKEND_EXECUTION_TRAP;
                candidate->failure.safepoint_id = 4u;
                descriptor->state_initialize(candidate->payload);
                binding =
                    xr_execution_lease_bind_state(&execution->lease, descriptor->state_layout,
                                                  candidate, native_state_destroy, &bound_state);
                if (binding != XR_EXECUTION_STATE_ADOPTED)
                    native_state_destroy(candidate);
            } else {
                xr_free(candidate);
            }
        }
    }
    if (binding != XR_EXECUTION_STATE_ADOPTED && binding != XR_EXECUTION_STATE_PRESENT) {
        xr_free(execution->frame);
        (void) xr_execution_lease_release(&execution->lease);
        xr_free(execution);
        return false;
    }
    execution->state = bound_state;
    XrBackendNativeHost host = {
        .context = execution,
        .output_write = native_output_write,
        .typed_call = native_typed_call,
        .typed_dispose = xr_execution_provider_result_dispose,
        .resource_free = xr_execution_resource_free,
    };
    descriptor->initialize(execution->frame, execution->state->payload, &host);
    execution->module_step = descriptor->module_step;
    execution->step = descriptor->step;
    execution->cancel = descriptor->cancel;
    execution->drop = descriptor->drop;
    execution->last.kind = XR_BACKEND_EXECUTION_INVALID;
    *execution_out = execution;
    return true;
}

static void native_initialization_fail(XrBackendExecution *execution,
                                       XrBackendNativeOutcome failure) {
    execution->state->failure = failure;
    execution->state->abort(execution->state->payload);
    (void) xr_execution_lease_initialization_finish(&execution->lease,
                                                    execution->initializer.module_index, false);
    execution->initializing = false;
}

static XrBackendExecutionOutcome backend_execution_outcome(XrBackendExecution *execution,
                                                           XrBackendNativeOutcome native) {
    XrBackendExecutionOutcome result = {
        .kind = native.kind <= XR_BACKEND_EXECUTION_TRAP
                    ? (XrBackendExecutionOutcomeKind) native.kind
                : native.kind == UINT32_C(3) ? XR_BACKEND_EXECUTION_CANCELLED
                : native.kind == XR_BACKEND_EXECUTION_ERROR ? XR_BACKEND_EXECUTION_ERROR
                : native.kind == XR_BACKEND_EXECUTION_PANIC ? XR_BACKEND_EXECUTION_PANIC
                                             : XR_BACKEND_EXECUTION_INVALID,
        .value = native.value,
        .state_id = native.state_id,
        .safepoint_id = native.safepoint_id,
        .suspension = native.suspension,
        .panic_present = native.panic_present,
        .panic_code = native.panic_code,
        .panic_has_bounds = native.panic_has_bounds,
        .panic_index = native.panic_index,
        .panic_length = native.panic_length,
        .panic_has_message = native.panic_has_message,
        .panic_message = native.panic_message,
        .panic_message_size = native.panic_message_size,
        .error_type_id = native.error_type_id,
        .error_value = native.error_value,
    };
    if (execution)
        execution->last = result;
    return result;
}

XrBackendExecutionOutcome xr_backend_execution_step(XrBackendExecution *execution) {
    if (!execution || execution->finished)
        return (XrBackendExecutionOutcome) {.kind = XR_BACKEND_EXECUTION_INVALID};
    for (;;) {
        if (!execution->initializing) {
            XrExecutionInitializationStatus status =
                xr_execution_lease_initialization_next(&execution->lease, &execution->initializer);
            if (status == XR_EXECUTION_INITIALIZATION_COMPLETE)
                break;
            if (status == XR_EXECUTION_INITIALIZATION_WAIT) {
                execution->last =
                    (XrBackendExecutionOutcome) {.kind = XR_BACKEND_EXECUTION_INITIALIZING};
                return execution->last;
            }
            if (status != XR_EXECUTION_INITIALIZATION_RUN) {
                execution->finished = true;
                return backend_execution_outcome(execution, execution->state->failure);
            }
            execution->initializing = true;
        }
        XrBackendNativeOutcome initialized =
            execution->module_step(execution->frame, execution->initializer.function_id, 0u);
        if (initialized.kind == XR_BACKEND_EXECUTION_SUSPENDED)
            return backend_execution_outcome(execution, initialized);
        if (initialized.kind != XR_BACKEND_EXECUTION_RETURN) {
            if (initialized.kind != XR_BACKEND_EXECUTION_TRAP &&
                initialized.kind != XR_BACKEND_EXECUTION_ERROR &&
                initialized.kind != XR_BACKEND_EXECUTION_PANIC)
                initialized = (XrBackendNativeOutcome) {.kind = XR_BACKEND_EXECUTION_TRAP,
                                                        .safepoint_id = 4u};
            native_initialization_fail(execution, initialized);
            execution->finished = true;
            return backend_execution_outcome(execution, initialized);
        }
        if (!xr_execution_lease_initialization_finish(&execution->lease,
                                                      execution->initializer.module_index, true)) {
            native_initialization_fail(
                execution,
                (XrBackendNativeOutcome) {.kind = XR_BACKEND_EXECUTION_TRAP, .safepoint_id = 4u});
            execution->finished = true;
            return backend_execution_outcome(execution, execution->state->failure);
        }
        execution->initializing = false;
    }
    execution->last = backend_execution_outcome(execution, execution->step(execution->frame));
    if (execution->last.kind != XR_BACKEND_EXECUTION_SUSPENDED) {
        execution->finished = true;
    }
    return execution->last;
}

XrBackendExecutionOutcome xr_backend_execution_cancel(XrBackendExecution *execution) {
    if (!execution || execution->finished || execution->last.kind != XR_BACKEND_EXECUTION_SUSPENDED)
        return (XrBackendExecutionOutcome) {.kind = XR_BACKEND_EXECUTION_INVALID};
    if (execution->initializing) {
        XrBackendNativeOutcome cancelled =
            execution->module_step(execution->frame, execution->initializer.function_id, 1u);
        native_initialization_fail(
            execution,
            cancelled.kind == XR_BACKEND_EXECUTION_TRAP
                ? cancelled
                : (XrBackendNativeOutcome) {.kind = XR_BACKEND_EXECUTION_TRAP, .safepoint_id = 4u});
        execution->last = backend_execution_outcome(execution, cancelled);
    } else {
        execution->last = backend_execution_outcome(execution, execution->cancel(execution->frame));
    }
    execution->finished = true;
    return execution->last;
}

void xr_backend_execution_free(XrBackendExecution *execution) {
    if (!execution)
        return;
    if (execution->initializing) {
        XrBackendNativeOutcome cancelled =
            execution->module_step(execution->frame, execution->initializer.function_id, 1u);
        native_initialization_fail(
            execution,
            cancelled.kind == XR_BACKEND_EXECUTION_TRAP
                ? cancelled
                : (XrBackendNativeOutcome) {.kind = XR_BACKEND_EXECUTION_TRAP, .safepoint_id = 4u});
    }
    execution->drop(execution->frame);
    xr_free(execution->frame);
    (void) xr_execution_lease_release(&execution->lease);
    xr_free(execution);
}
