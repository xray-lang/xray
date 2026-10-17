/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_native_source.inc.c - Borrow only the current native value region
 */
XR_FUNC XrXirCallStatus xir_call_native_source_before(XrXirCallView *view,uint32_t instruction,
    const unsigned char *values,uint32_t bytes,bool *stopped) {
    if (!stopped || !values || !xr_xir_call_admission(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCall *call=view->activation;CallFrame *frame=call->top;
    if (frame->code_kind!=XR_XIR_CALL_CODE_NATIVE) return XR_XIR_CALL_BAD_STATE;
    uintptr_t base=(uintptr_t)frame->state,address=(uintptr_t)values;
    if (address<base || address-base>frame->entry->state_bytes ||
        bytes>frame->entry->state_bytes-(address-base)) return XR_XIR_CALL_BAD_STATE;
    if (view->phase!=XR_XIR_CALL_NORMAL || cleanup_active(call) || call->cancel_requested || call->aborting) {
        *stopped=false;return XR_XIR_CALL_READY;
    }
    XrXirCallBudget *budget=call->budget;
    if (!budget || !budget->native_source) { *stopped=false;return XR_XIR_CALL_READY; }
    if (budget->native_source_instance!=call->config.instance || !call->executor_owner ||
        !call->executor_activation || !call->executor_generation || !call->executor_ticket) return XR_XIR_CALL_BAD_STATE;
    XirCallNativeSourcePoint point={view,values,bytes,(uint32_t)(frame->entry-call->config.entries),instruction,frame->epoch};
    bool fresh=false;XrXirCallStatus status=budget->native_source->before(budget->native_source->owner,&point,&fresh);
    if (status==XR_XIR_CALL_READY) *stopped=fresh;
    return status;
}
