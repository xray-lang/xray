/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_result.inc.c - Complete ownership of ordinary and panic outcomes
 *
 * KEY CONCEPT:
 *   A result owns exactly one outcome domain; borrowed poll views do not drop.
 */
XR_FUNCDEF bool xr_xir_call_result_empty(const XrXirCallResult *result) {
    return result && result->status == XR_XIR_CALL_READY && !result->wake &&
        boundary_value(result->value, XR_XIR_UNIT) && xr_xir_panic_empty(&result->panic);
}
XR_FUNCDEF bool xr_xir_call_result_valid(const XrXirCallResult *result) {
    if (!result || result->status < XR_XIR_CALL_READY || result->status > XR_XIR_CALL_UNSUPPORTED ||
        !xr_xir_value_valid(&result->value)) return false;
    if (xr_xir_call_panic_status(result->status))
        return !result->wake && boundary_value(result->value, XR_XIR_UNIT) &&
            xr_xir_call_panic_payload(result->status, &result->panic);
    if (!xr_xir_panic_empty(&result->panic)) return false;
    if (result->status == XR_XIR_CALL_RETURNED) return !result->wake;
    if (result->status == XR_XIR_CALL_THROWN) {
        const XrXirTypeArena *arena = xr_xir_value_arena(&result->value);
        return !result->wake && (result->value.type == XR_XIR_ERROR ||
            xr_xir_type_is_enum(xr_xir_compile_type_arena_types(arena), (XrXirType)result->value.type));
    }
    return boundary_value(result->value, XR_XIR_UNIT) &&
        (result->status == XR_XIR_CALL_SUSPENDED ? result->wake != 0 : result->wake == 0);
}
XR_FUNCDEF XrXirValueStatus xr_xir_call_result_copy(const XrXirCallResult *source, XrXirCallResult *output) {
    if (!xr_xir_call_result_empty(output) || !xr_xir_call_result_valid(source))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirCallResult copy = call_result(source->status);
    copy.wake = source->wake;
    XrXirValueStatus status = xr_xir_value_copy(&source->value, &copy.value);
    if (status != XR_XIR_VALUE_OK) return status;
    status = xr_xir_panic_copy(&source->panic, &copy.panic);
    if (status != XR_XIR_VALUE_OK) { xr_xir_value_drop(&copy.value); return status; }
    *output = copy;
    return XR_XIR_VALUE_OK;
}
XR_FUNCDEF void xr_xir_call_result_move(XrXirCallResult *source, XrXirCallResult *output) {
    XR_CHECK(source != output && xr_xir_call_result_valid(source) && xr_xir_call_result_empty(output),
        "result move requires a valid owner and empty destination");
    *output = *source;
    *source = call_result(XR_XIR_CALL_READY);
}
XR_FUNCDEF void xr_xir_call_result_drop(XrXirCallResult *result) {
    if (!result) return;
    XR_CHECK(xr_xir_call_result_valid(result), "invalid owned result release");
    xr_xir_value_drop(&result->value);
    xr_xir_panic_drop(&result->panic);
    *result = call_result(XR_XIR_CALL_READY);
}
XR_FUNCDEF XrXirCallStatus xr_xir_call_discard_inbox(XrXirCallView *view, XrXirType expected) {
    if (!xr_xir_call_admission(view) || view->phase != XR_XIR_CALL_NORMAL || view->scope_exit)
        return XR_XIR_CALL_BAD_STATE;
    CallFrame *frame = view->activation->top;
    const XrXirCallResult *owner = &frame->inbox;
    if (frame->exiting || owner->status != XR_XIR_CALL_RETURNED || view->inbox.status != owner->status ||
        !xr_xir_call_result_valid(owner) || !xr_xir_call_result_valid(&view->inbox) ||
        owner->wake != view->inbox.wake || !xr_xir_panic_empty(&view->inbox.panic) ||
        owner->value.type != view->inbox.value.type || owner->value.reserved != view->inbox.value.reserved ||
        owner->value.payload != view->inbox.value.payload ||
        (expected == XR_XIR_UNIT ? !boundary_value(owner->value,expected) :
            !xr_xir_value_argument(&owner->value,view->arena,expected))) return XR_XIR_CALL_BAD_STATE;
    xr_xir_call_result_drop(&frame->inbox);
    view->inbox = call_result(XR_XIR_CALL_READY);
    return XR_XIR_CALL_READY;
}
