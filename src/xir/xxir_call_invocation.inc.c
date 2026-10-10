/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_invocation.inc.c - Transactional current-frame equation selection
 *
 * KEY CONCEPT:
 *   Staging names a frontier only. A live Instance rechecks the action's actual
 *   inputs before a completed selection moves into a committed callee frame.
 */
static bool call_invocation_work(XrXirCallView *view,uint64_t work) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || !admission->domain || work>admission->work ||
        !xr_xir_domain_work(admission->domain,work)) return false;
    admission->work-=work;return true;
}
XR_FUNC const XirEffectInvocationSelection *xr_xir_call_invocation(const XrXirCallView *view) {
    return xr_xir_call_admission(view) && view->activation->top->invocation.owner?
        &view->activation->top->invocation:NULL;
}
XR_FUNC bool xr_xir_call_invocation_pending(const XrXirCallView *view,uint32_t *instruction) {
    if (!instruction || !xr_xir_call_admission(view) || !view->activation->top->effect_pending) return false;
    *instruction=view->activation->top->effect_instruction;return true;
}
XR_FUNC XrXirCallStatus xr_xir_call_invocation_stage(XrXirCallView *view,uint32_t instruction) {
    if (!xr_xir_call_admission(view) || instruction==UINT32_MAX) return XR_XIR_CALL_BAD_STATE;
    CallFrame *frame=view->activation->top;
    if (frame->effect_pending || frame->pending_invocation.owner) return XR_XIR_CALL_BAD_STATE;
    if (!call_invocation_work(view,2)) return XR_XIR_CALL_LIMIT;
    frame->effect_instruction=instruction;frame->effect_pending=true;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_call_invocation_accept(XrXirCallView *view,
    const XirEffectInvocationSelection *selection) {
    if (!xr_xir_call_admission(view) || !selection || !selection->owner) return XR_XIR_CALL_BAD_STATE;
    CallFrame *frame=view->activation->top;
    if (!frame->effect_pending || frame->pending_invocation.owner ||
        frame->effect_instruction!=selection->instruction ||
        selection->caller!=(uint32_t)(frame->entry-view->activation->config.entries))
        return XR_XIR_CALL_BAD_STATE;
    if (!call_invocation_work(view,sizeof(*selection)+4)) return XR_XIR_CALL_LIMIT;
    frame->pending_invocation=*selection;return XR_XIR_CALL_READY;
}
