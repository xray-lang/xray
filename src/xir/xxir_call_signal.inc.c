/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_signal.inc.c - Direct-call origin transport without permissions
 */
XR_FUNC XrXirCallStatus xir_call_signal_stage(XrXirCallView *view,uint32_t instruction,uint32_t target) {
    if (!view || !call_owner_current(view->activation) || !xr_xir_call_admission(view))
        return XR_XIR_CALL_BAD_ARGUMENT;
    CallFrame *frame=view->activation->top;
    if (view->phase!=XR_XIR_CALL_NORMAL || view->scope_exit || !frame || frame->exiting ||
        frame->in_cleanup || frame->cleanup_call || frame->signal_staged ||
        instruction==UINT32_MAX || target>=frame->metadata->entry_count)
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status=xr_xir_call_execution_status(view);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!call_invocation_work(view,sizeof(XirCallSignalOrigin)+1)) return XR_XIR_CALL_LIMIT;
    frame->signal_pending=(XirCallSignalOrigin){
        (uint32_t)(frame->entry-frame->metadata->entries),instruction,target};
    frame->signal_staged=true;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xir_call_signal_origin(XrXirCallView *view,XirCallSignalOrigin *output) {
    if (!view || !output || !call_owner_current(view->activation) || !xr_xir_call_admission(view))
        return XR_XIR_CALL_BAD_ARGUMENT;
    CallFrame *frame=view->activation->top;
    if (!frame || !frame->signal_available) return XR_XIR_CALL_BAD_STATE;
    /* Consume before inspecting: malformed or interrupted attempts cannot replay
     * this native frame's origin, even when a caller repeats the private entry. */
    frame->signal_available=false;
    const CallFrame *parent=frame->parent;
    if (!parent || view->phase!=XR_XIR_CALL_NORMAL || view->scope_exit || frame->exiting ||
        frame->in_cleanup || frame->cleanup_call || frame->metadata!=parent->metadata ||
        parent->epoch!=frame->signal_parent_epoch || parent->exiting || parent->in_cleanup ||
        parent->cleanup_call || frame->signal_origin.caller!=(uint32_t)(parent->entry-parent->metadata->entries) ||
        frame->signal_origin.target!=(uint32_t)(frame->entry-frame->metadata->entries))
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallStatus status=xr_xir_call_execution_status(view);
    if (status!=XR_XIR_CALL_READY) return status;
    *output=frame->signal_origin;return XR_XIR_CALL_READY;
}
