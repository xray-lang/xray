/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_native_observer.inc.c - Own the native throw before any unwind
 */
XR_FUNC XrXirCallStatus xir_call_native_observer_bind(const XrXirCallView *view,const XirCallNativeObserver *observer) {
    if (!view || !observer || !observer->owner || !observer->throwing) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_STATE;
    XrXirCall *call=view->activation;XrXirCallBudget *budget=call->budget;bool child=false;
    if (!budget || !call->config.instance || !call->executor_owner ||
        !xr_xir_task_executor_view_member(call->executor_owner,view,&child)) return XR_XIR_CALL_BAD_STATE;
    if (budget->native_observer) return budget->native_observer==observer &&
        budget->native_instance==call->config.instance ? XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_domain_work(budget->work_domain,1)) return XR_XIR_CALL_LIMIT;
    budget->native_instance=call->config.instance;budget->native_observer=observer;return XR_XIR_CALL_READY;
}
static XrXirCallStatus call_native_throw_offer(XrXirCall *call,XrXirCallResult *result,bool *held) {
    XrXirCallBudget *budget=call->budget;CallFrame *frame=call->top;
    if (!budget || !budget->native_observer) return XR_XIR_CALL_READY;
    if (budget->native_instance!=call->config.instance || !call->executor_owner ||
        !call->executor_activation || !call->executor_generation || !call->executor_ticket) return XR_XIR_CALL_BAD_STATE;
    if (budget->native_hold || frame->native_pending || frame->exiting || result->status!=XR_XIR_CALL_THROWN)
        return XR_XIR_CALL_BAD_STATE;
    if (!xr_xir_domain_work(budget->work_domain,sizeof(XirCallNativeThrowEvent))) return XR_XIR_CALL_LIMIT;
    XirCallNativeThrowEvent event={{{call->executor_owner,call->executor_activation,
        call->executor_generation,call->executor_ticket},call->config.instance,frame->epoch,
        frame->parent ? frame->parent->epoch : 0,(uint32_t)(frame->entry-call->config.entries),frame->depth,
        XR_XIR_CALL_NORMAL},&result->value};
    bool pause=false;XrXirCallView *active=call->active_view;
    call->active_view=NULL;budget->notifying=true;
    XrXirCallStatus status=budget->native_observer->throwing(budget->native_observer->owner,&event,&pause);
    budget->notifying=false;call->active_view=active;
    if (status!=XR_XIR_CALL_READY || !pause) return status;
    xr_xir_call_result_move(result,&frame->pending);frame->native_pending=true;
    budget->native_hold=call;*held=true;return XR_XIR_CALL_READY;
}
static XrXirCallStatus call_native_throw_current(XrXirCall *call,const XrXirExecutorBinding *binding,uint64_t identity) {
    if (!call_owner_current(call) || !call || !binding || !identity) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call) || call->cleaning || call->admitting) return XR_XIR_CALL_BUSY;
    if (!xr_xir_call_executor_member(call,binding) || !call->budget || call->budget->native_hold!=call ||
        !call->top || call->top->epoch!=identity || !call->top->native_pending || call->top->exiting ||
        call->top->code_kind!=XR_XIR_CALL_CODE_NATIVE || call->top->pending.status!=XR_XIR_CALL_THROWN ||
        call->cancel_requested || call->aborting || call->result.status!=XR_XIR_CALL_READY) return XR_XIR_CALL_BAD_STATE;
    return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xir_call_native_throw_copy(XrXirCall *call,const XrXirExecutorBinding *binding,
    uint64_t identity,XirCallNativeThrow *output) {
    if (!output || !boundary_value(output->value,XR_XIR_UNIT)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallStatus status=call_native_throw_current(call,binding,identity);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!xr_xir_domain_work(call->budget->work_domain,sizeof(*output))) return XR_XIR_CALL_LIMIT;
    CallFrame *frame=call->top;XirCallNativeThrow fresh={{{call->executor_owner,call->executor_activation,
        call->executor_generation,call->executor_ticket},call->config.instance,frame->epoch,
        frame->parent ? frame->parent->epoch : 0,(uint32_t)(frame->entry-call->config.entries),frame->depth,
        XR_XIR_CALL_NORMAL},{0}};
    if (xr_xir_value_copy(&frame->pending.value,&fresh.value)!=XR_XIR_VALUE_OK) return XR_XIR_CALL_LIMIT;
    *output=fresh;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xir_call_native_throw_continue(XrXirCall *call,const XrXirExecutorBinding *binding,uint64_t identity) {
    XrXirCallStatus status=call_native_throw_current(call,binding,identity);
    if (status!=XR_XIR_CALL_READY) return status;
    call->budget->native_hold=NULL;return XR_XIR_CALL_READY;
}
