/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_exit.inc.c - Owned pending exits on the single call trampoline
 *
 * KEY CONCEPT:
 *   Exit values outlive their producer slots and cleanup never replaces them.
 */
static void request_exit(XrXirCall *call, XrXirCallResult result, bool scope) {
    CallFrame *frame = call->top;
    xr_xir_value_drop(&frame->pending.value);
    frame->pending = result;
    frame->exiting = true;
    frame->scope_exit = scope;
}
static const char *exit_message(XrXirCallResult result, char *buffer, size_t capacity) {
    if (!xr_xir_call_panic_status(result.status)) return XR_ERROR_CORE_NO_MESSAGE_MSG;
    int prefix = snprintf(buffer, capacity, "E%04u: ", result.fault.code);
    if (prefix <= 0 || (size_t)prefix >= capacity ||
        !xr_xir_panic_message_format(result.fault, buffer + prefix, capacity - (size_t)prefix))
        return XR_ERROR_CORE_NO_MESSAGE_MSG;
    return buffer;
}
static void cleanup_escape(XrXirCall *call, XrXirCallResult escaped) {
    char message[XR_XIR_PANIC_MESSAGE_CAPACITY + 16], pending[XR_XIR_PANIC_MESSAGE_CAPACITY + 16];
    const char *in_flight = NULL;
    for (CallFrame *parent = call->top->parent; parent; parent = parent->parent) {
        if (parent->exiting && (parent->pending.status == XR_XIR_CALL_THROWN ||
            xr_xir_call_panic_status(parent->pending.status))) {
            in_flight = exit_message(parent->pending, pending, sizeof(pending));
            break;
        }
    }
    xr_error_core_defer_throw_abort(443, exit_message(escaped, message, sizeof(message)), in_flight);
}
static void finish_exit(XrXirCall *call) {
    CallFrame *frame = call->top;
    XrXirCallResult result = frame->pending;
    if (frame->cleanup_call && !frame->scope_exit && (result.status == XR_XIR_CALL_THROWN || xr_xir_call_panic_status(result.status)))
        cleanup_escape(call, result);
    frame->pending = call_result(XR_XIR_CALL_READY);
    if (frame->scope_exit) {
        frame->exiting = frame->scope_exit = false;
        xr_xir_value_drop(&frame->inbox.value);
        frame->inbox = result;
        if (call->cancel_requested && !frame->in_cleanup)
            request_exit(call, call_result(XR_XIR_CALL_CANCELLED), false);
        return;
    }
    if (call->cancel_requested && !frame->in_cleanup) {
        xr_xir_value_drop(&result.value);
        result = call_result(XR_XIR_CALL_CANCELLED);
    }
    pop_frame(call, result.status);
    if (!call->top) { call->result = result; return; }
    if (result.status == XR_XIR_CALL_CANCELLED ||
        (xr_xir_call_panic_status(result.status) && !call->top->protected_call)) {
        request_exit(call, result, false);
        return;
    }
    xr_xir_value_drop(&call->top->inbox.value);
    call->top->inbox = result;
    call->top->protected_call = false;
}

static void accept_scope_exit(XrXirCall *call, XrXirAction action, bool panic) {
    if (call->top->exiting || !(call->top->entry->flags & XR_XIR_ENTRY_EXIT)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    XrXirCallResult result = call_result(XR_XIR_CALL_READY);
    if (action.flags == XR_XIR_ACTION_LEAVE_PANIC && panic) {
        result.status = (XrXirCallStatus)action.value.payload; result.fault = action.fault;
    } else if (action.flags == XR_XIR_ACTION_LEAVE_ERROR) {
        XrXirType type = (XrXirType)action.value.type;
        if (!xr_xir_type_is_enum(xr_xir_type_arena_types(call->config.admission.arena), type)) {
            abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
        }
        call->admitting = true;
        XrXirCallStatus status = admit_value(&action.value, type, &call->config.admission);
        call->admitting = false;
        if (status != XR_XIR_CALL_READY) {
            abort_frames(call, status == XR_XIR_CALL_BAD_ARGUMENT ? XR_XIR_CALL_BAD_STATE : status); return;
        }
        result.status = XR_XIR_CALL_THROWN;
        if (xr_xir_value_copy(&action.value, &result.value) != XR_XIR_VALUE_OK) {
            abort_frames(call, XR_XIR_CALL_LIMIT); return;
        }
    } else if (action.flags || !boundary_value(action.value, XR_XIR_UNIT)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    request_exit(call, result, true);
}
