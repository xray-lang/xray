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
static void request_exit(XrXirCall *call, XrXirCallResult *result, bool scope) {
    CallFrame *frame = call->top;
    xr_xir_call_result_drop(&frame->pending);
    xr_xir_call_result_move(result, &frame->pending);
    frame->exiting = true;
    frame->exit_done = false;
    frame->scope_exit = scope;
}
static void request_exit_status(XrXirCall *call, XrXirCallStatus status) {
    XrXirCallResult result = call_result(status);
    request_exit(call, &result, false);
}
static XrErrorCoreMessageView exit_message(const XrXirCallResult *result, char *buffer, size_t capacity) {
    XrErrorCoreMessageView message = {0};
    if (xr_xir_call_panic_status(result->status))
        xr_xir_panic_message_borrow(&result->panic, buffer, capacity, &message);
    return message;
}
static void cleanup_escape(XrXirCall *call, XrXirCallResult escaped) {
    char message[XR_XIR_PANIC_MESSAGE_CAPACITY], pending[XR_XIR_PANIC_MESSAGE_CAPACITY];
    XrErrorCoreMessageView in_flight = {0};
    bool found = false;
    for (CallFrame *parent = call->top->parent; parent; parent = parent->parent) {
        if (parent->exiting && (parent->pending.status == XR_XIR_CALL_THROWN ||
            xr_xir_call_panic_status(parent->pending.status))) {
            in_flight = exit_message(&parent->pending, pending, sizeof(pending));
            found = true;
            break;
        }
    }
    xr_error_core_defer_throw_abort(443, exit_message(&escaped, message, sizeof(message)), found ? &in_flight : NULL);
}
static void finish_exit(XrXirCall *call) {
    CallFrame *frame = call->top;
    XrXirCallResult result = call_result(XR_XIR_CALL_READY);
    xr_xir_call_result_move(&frame->pending, &result);
    if (frame->cleanup_call && !frame->scope_exit && (result.status == XR_XIR_CALL_THROWN || xr_xir_call_panic_status(result.status)))
        cleanup_escape(call, result);
    if (frame->scope_exit) {
        frame->exiting = frame->scope_exit = frame->exit_done = false;
        xr_xir_call_result_drop(&frame->inbox);
        xr_xir_call_result_move(&result, &frame->inbox);
        if (call->cancel_requested && !frame->in_cleanup)
            request_exit_status(call, XR_XIR_CALL_CANCELLED);
        return;
    }
    if (call->cancel_requested && !frame->in_cleanup) {
        xr_xir_call_result_drop(&result);
        result = call_result(XR_XIR_CALL_CANCELLED);
    }
    pop_frame(call, result.status);
    if (!call->top) {
        xr_xir_call_result_drop(&call->result);
        xr_xir_call_result_move(&result, &call->result);
        return;
    }
    if (result.status == XR_XIR_CALL_CANCELLED ||
        (xr_xir_call_panic_status(result.status) && !call->top->protected_call)) {
        request_exit(call, &result, false);
        return;
    }
    xr_xir_call_result_drop(&call->top->inbox);
    xr_xir_call_result_move(&result, &call->top->inbox);
    call->top->protected_call = false;
}

static void accept_scope_exit(XrXirCall *call, XrXirAction action, bool panic) {
    if (call->top->exiting || !(call->top->entry->flags & XR_XIR_ENTRY_EXIT)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    XrXirCallResult result = call_result(XR_XIR_CALL_READY);
    if (action.flags == XR_XIR_ACTION_LEAVE_PANIC && panic) {
        result.status = (XrXirCallStatus)action.value.payload;
        if (xr_xir_panic_copy(&action.panic, &result.panic) != XR_XIR_VALUE_OK) {
            abort_frames(call, XR_XIR_CALL_LIMIT); return;
        }
    } else if (action.flags == XR_XIR_ACTION_LEAVE_ERROR) {
        XrXirType type = (XrXirType)action.value.type;
        if (!xr_xir_type_is_enum(xr_xir_compile_type_arena_types(call->config.admission.arena), type)) {
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
    request_exit(call, &result, true);
}
