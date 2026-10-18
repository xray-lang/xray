/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_native_source.inc.c - Borrow only the current native value region
 */
static XrXirCallStatus call_native_source_observer(XrXirCallView *view,const XirCallNativeSourceObserver **output) {
    if (!xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call=view->activation;CallFrame *frame=call->top;
    if (frame->code_kind==XR_XIR_CALL_CODE_VM) return XR_XIR_CALL_BAD_STATE;
    *output=NULL;
    if (view->phase!=XR_XIR_CALL_NORMAL || cleanup_active(call) || call->cancel_requested || call->aborting)
        return XR_XIR_CALL_READY;
    XrXirCallBudget *budget=call->budget;
    if (!budget || !budget->native_source) return XR_XIR_CALL_READY;
    if (budget->native_source_instance!=call->config.instance || !call->executor_owner ||
        !call->executor_activation || !call->executor_generation || !call->executor_ticket) return XR_XIR_CALL_BAD_STATE;
    if (frame->code_kind==XR_XIR_CALL_CODE_UNKNOWN) {
        frame->code_kind=XR_XIR_CALL_CODE_NATIVE;
        call_observer_event(call,frame,XR_XIR_CALL_FRAME_CODE,XR_XIR_CALL_READY);
    }
    *output=budget->native_source;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xir_call_native_source_before(XrXirCallView *view,uint32_t instruction,
    const unsigned char *values,uint32_t bytes,bool *stopped) {
    if (!stopped || !values) return XR_XIR_CALL_BAD_ARGUMENT;
    const XirCallNativeSourceObserver *observer=NULL;
    XrXirCallStatus status=call_native_source_observer(view,&observer);
    if (status!=XR_XIR_CALL_READY) return status;
    CallFrame *frame=view->activation->top;
    uintptr_t base=(uintptr_t)frame->state,address=(uintptr_t)values;
    if (address<base || address-base>frame->entry->state_bytes ||
        bytes>frame->entry->state_bytes-(address-base)) return XR_XIR_CALL_BAD_STATE;
    if (!observer) { *stopped=false;return XR_XIR_CALL_READY; }
    XirCallNativeSourcePoint point={view,values,bytes,(uint32_t)(frame->entry-view->activation->config.entries),instruction,frame->epoch};
    bool fresh=false;status=observer->before(observer->owner,&point,&fresh);
    if (status==XR_XIR_CALL_READY) *stopped=fresh;
    return status;
}
XR_FUNC XrXirCallStatus xir_call_native_source_call(XrXirCallView *view,uint32_t instruction,uint32_t callee,bool invoked) {
    const XirCallNativeSourceObserver *observer=NULL;
    XrXirCallStatus status=call_native_source_observer(view,&observer);
    if (status!=XR_XIR_CALL_READY || !observer) return status;
    XrXirCall *call=view->activation;CallFrame *frame=call->top;
    if (callee>=call->config.entry_count || !observer->call) return XR_XIR_CALL_BAD_STATE;
    XirCallNativeSourcePending point={view,frame->epoch,(uint32_t)(frame->entry-call->config.entries),instruction,callee,invoked};
    return observer->call(observer->owner,&point);
}
