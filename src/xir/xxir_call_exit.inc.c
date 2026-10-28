/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_exit.inc.c - Owned pending exits on the single call trampoline
 *
 * KEY CONCEPT:
 *   Owned exit values outlive producer slots. The first execution failure
 *   survives cleanup failures and later cancellation.
 */
static void request_exit(XrXirCall *call, XrXirCallResult *result, bool scope) {
    clear_channel_wait(call);
    CallFrame *frame = call->top;
    if (call->budget && call->budget->native_hold==call) call->budget->native_hold=NULL;
    frame->native_pending=false;
    if (result->status == XR_XIR_CALL_CANCELLED && call->abort_reason != XR_XIR_CALL_READY) {
        xr_xir_call_result_drop(result);
        *result = call_result(call->abort_reason);
    }
    xr_xir_call_result_drop(&frame->pending);
    frame->return_staged = false;
    xr_xir_call_result_move(result, &frame->pending);
    frame->exiting = true;
    frame->exit_done = false;
    frame->scope_exit = scope;
}
static void request_exit_status(XrXirCall *call, XrXirCallStatus status) {
    XrXirCallResult result = call_result(status);
    request_exit(call, &result, false);
}
/* Only a validated execution failure may use intact frames for language EXIT.
 * Protocol rejection keeps the separate physical-abort path. */
static void request_execution_failure(XrXirCall *call, XrXirCallStatus reason) {
    if (call->abort_reason == XR_XIR_CALL_READY) call->abort_reason = reason;
    request_exit_status(call, call->abort_reason);
}
/* A cleanup panic replaces a business exit, but a second panic/cancel unwind
 * becomes an owned physical-close terminal. The guest cannot name this state. */
static bool cleanup_escape(XrXirCall *call,XrXirCallResult *escaped) {
    CallFrame *parent=call->top->parent;
    if (!parent || !parent->exiting || !xr_xir_call_panic_status(escaped->status)) {
        xr_xir_call_result_drop(escaped);abort_frames(call,XR_XIR_CALL_BAD_STATE);return false;
    }
    if (call->abort_reason!=XR_XIR_CALL_READY) {
        xr_xir_call_result_drop(escaped);abort_frames(call,call->abort_reason);return false;
    }
    if (xr_xir_call_panic_status(parent->pending.status) || parent->pending.status==XR_XIR_CALL_CANCELLED) {
        XrXirCallResult fatal=call_result(XR_XIR_CALL_CLEANUP_FAILED);
        fatal.prior_status=parent->pending.status;
        xr_xir_panic_move(&escaped->panic,&fatal.panic);escaped->status=XR_XIR_CALL_READY;
        if (xr_xir_call_panic_status(parent->pending.status)) {
            xr_xir_panic_move(&parent->pending.panic,&fatal.prior_panic);
            parent->pending.status=XR_XIR_CALL_READY;
        }
        xr_xir_call_result_drop(&parent->pending);
        xr_xir_call_result_move(&fatal,&call->abort_outcome);
        abort_frames(call,XR_XIR_CALL_CLEANUP_FAILED);return false;
    }
    if (parent->pending.status==XR_XIR_CALL_THROWN) {
        XrXirValue error={0};call->admitting=true;
        XrXirValueStatus status=xr_xir_error_erase(&parent->pending.value,&call->config.admission,&error);
        call->admitting=false;
        if (status!=XR_XIR_VALUE_OK) {
            xr_xir_call_result_drop(escaped);
            if (status==XR_XIR_VALUE_OOM || status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT)
                request_execution_failure(call,status==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_LIMIT);
            else abort_frames(call,XR_XIR_CALL_BAD_STATE);
            return false;
        }
        xr_xir_value_drop(&escaped->panic.replaced_error);
        xr_xir_value_move(&error,&escaped->panic.replaced_error);
    }
    XrXirCallResult pending=call_result(XR_XIR_CALL_READY);
    XrXirValueStatus copied=xr_xir_call_result_copy(escaped,&pending);
    if (copied!=XR_XIR_VALUE_OK) {
        xr_xir_call_result_drop(escaped);
        request_execution_failure(call,copied==XR_XIR_VALUE_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_LIMIT);return false;
    }
    xr_xir_call_result_drop(&parent->pending);
    xr_xir_call_result_move(&pending,&parent->pending);
    return true;
}
static void finish_exit(XrXirCall *call) {
    CallFrame *frame = call->top;
    XrXirCallResult result = call_result(XR_XIR_CALL_READY);
    xr_xir_call_result_move(&frame->pending, &result);
    if (frame->cleanup_call && !frame->scope_exit &&
        (result.status==XR_XIR_CALL_THROWN || xr_xir_call_panic_status(result.status))) {
        if (!cleanup_escape(call,&result)) return;
        pop_frame(call,result.status);
        xr_xir_call_result_drop(&call->top->inbox);
        xr_xir_call_result_move(&result,&call->top->inbox);call->top->inbox_dispatched=false;
        return;
    }
    if (frame->scope_exit) {
        frame->exiting = frame->scope_exit = frame->exit_done = false;
        xr_xir_call_result_drop(&frame->inbox);
        xr_xir_call_result_move(&result, &frame->inbox);frame->inbox_dispatched=false;
        if (call->cancel_requested && !frame->in_cleanup && call->abort_reason == XR_XIR_CALL_READY &&
            !xr_xir_call_panic_status(frame->inbox.status))
            request_exit_status(call, XR_XIR_CALL_CANCELLED);
        return;
    }
    if (call->cancel_requested && !frame->in_cleanup && call->abort_reason == XR_XIR_CALL_READY &&
            !xr_xir_call_panic_status(result.status)) {
        xr_xir_call_result_drop(&result);
        result = call_result(XR_XIR_CALL_CANCELLED);
    }
    bool execution_failure = call->abort_reason != XR_XIR_CALL_READY && result.status == call->abort_reason;
    bool failed_cleanup = frame->cleanup_call && execution_failure;
    pop_frame(call, result.status);
    if (!call->top) {
        xr_xir_call_result_drop(&call->result);
        xr_xir_call_result_move(&result, &call->result);
        return;
    }
    if (failed_cleanup) {
        xr_xir_call_result_drop(&result);
        abort_frames(call, call->abort_reason);
        return;
    }
    if (execution_failure || result.status == XR_XIR_CALL_CANCELLED ||
        (xr_xir_call_panic_status(result.status) && !call->top->protected_call)) {
        request_exit(call, &result, false);
        return;
    }
    xr_xir_call_result_drop(&call->top->inbox);
    xr_xir_call_result_move(&result, &call->top->inbox);call->top->inbox_dispatched=false;
    call->top->protected_call = false;
}

static void accept_scope_exit(XrXirCall *call, XrXirAction action, bool panic) {
    if (call->top->exiting || !(call->top->entry->flags & XR_XIR_ENTRY_EXIT)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    XrXirCallResult result = call_result(XR_XIR_CALL_READY);
    if (action.flags == XR_XIR_ACTION_LEAVE_PANIC && panic) {
        call->admitting=true;
        XrXirCallStatus admitted=admit_panic(&action.panic,&call->config.admission);
        call->admitting=false;
        if (admitted!=XR_XIR_CALL_READY) {
            if (admitted==XR_XIR_CALL_OOM || admitted==XR_XIR_CALL_LIMIT) request_execution_failure(call,admitted);
            else abort_frames(call,XR_XIR_CALL_BAD_STATE);
            return;
        }
        result.status = (XrXirCallStatus)action.value.payload;
        if (xr_xir_panic_copy(&action.panic, &result.panic) != XR_XIR_VALUE_OK) {
            request_execution_failure(call, XR_XIR_CALL_LIMIT); return;
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
            if (status == XR_XIR_CALL_OOM || status == XR_XIR_CALL_LIMIT)
                request_execution_failure(call, status);
            else abort_frames(call, status == XR_XIR_CALL_BAD_ARGUMENT ? XR_XIR_CALL_BAD_STATE : status);
            return;
        }
        result.status = XR_XIR_CALL_THROWN;
        if (xr_xir_value_copy(&action.value, &result.value) != XR_XIR_VALUE_OK) {
            request_execution_failure(call, XR_XIR_CALL_LIMIT); return;
        }
    } else if (action.flags || !boundary_value(action.value, XR_XIR_UNIT)) {
        abort_frames(call, XR_XIR_CALL_BAD_STATE); return;
    }
    if (result.status==XR_XIR_CALL_THROWN && call->top->code_kind==XR_XIR_CALL_CODE_NATIVE && !cleanup_active(call)) {
        bool held=false;XrXirCallStatus observed=call_native_throw_offer(call,&result,true,&held);
        if (observed!=XR_XIR_CALL_READY) {
            xr_xir_call_result_drop(&result);
            if (observed==XR_XIR_CALL_OOM || observed==XR_XIR_CALL_LIMIT) request_execution_failure(call,observed);
            else abort_frames(call,XR_XIR_CALL_BAD_STATE);
            return;
        }
        if (held) return;
    }
    request_exit(call, &result, true);
}
