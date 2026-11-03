/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_call_signal_frontier.inc.c - Observe every live interruption obligation
 *
 * KEY CONCEPT:
 *   No frame is revoked, resumed or reclassified while observing eligibility.
 *   Parent frames retain their loans and promises while a child is current.
 */
XR_FUNC XrXirCallStatus xir_call_signal_frontier(const XrXirCall *call,bool *eligible) {
    if (!call || !eligible || !call_owner_current(call)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (call->driving || call_observing(call) || call->cleaning || call->admitting || call->active_view)
        return XR_XIR_CALL_BUSY;
    *eligible=false;
    if (!call->top || (call->result.status!=XR_XIR_CALL_READY && call->result.status!=XR_XIR_CALL_SUSPENDED) ||
        call->cancel_requested || call->aborting || call->abort_reason!=XR_XIR_CALL_READY ||
        call->cleanup_incomplete || (call->budget && call->budget->exhausted)) return XR_XIR_CALL_READY;
    uint32_t remaining=call->top->depth;
    for (const CallFrame *frame=call->top;frame;frame=frame->parent) {
        if (!remaining || frame->depth!=remaining-- || !frame->epoch || !frame->metadata ||
            frame->exiting || frame->in_cleanup || frame->cleanup_call || frame->scope_exit ||
            frame->scope_draining || frame->scope_resume || frame->return_staged || frame->native_pending ||
            (frame->pending.status!=XR_XIR_CALL_READY && frame->pending.status!=XR_XIR_CALL_RETURNED) ||
            (frame->inbox.status!=XR_XIR_CALL_READY && frame->inbox.status!=XR_XIR_CALL_RETURNED))
            return XR_XIR_CALL_READY;
        for (uint32_t i=0;i<frame->loan_count;++i) {
            const XrXirCellLoan *loan=&frame->loans[i].loan;
            if (loan->cell || loan->scope || loan->child || loan->authority.activation)
                return XR_XIR_CALL_READY;
        }
        for (const CallViewBinding *view=frame->views;view;view=view->next)
            if (xr_xir_view_scope_active(&view->pin)) return XR_XIR_CALL_READY;
        bool no_suspend=false;
        uint32_t entry=(uint32_t)(frame->entry-frame->metadata->entries);
        if (!xir_instance_signal_frame(call->config.instance,call,&call->config.admission,
                frame->metadata,entry,&no_suspend) || no_suspend) return XR_XIR_CALL_READY;
    }
    *eligible=remaining==0;
    return XR_XIR_CALL_READY;
}
