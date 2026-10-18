/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_panic_flow.inc.c - Bounded panic cleanup and frame propagation
 *
 * KEY CONCEPT:
 *   Each activation owns its pending panic independently. Cleanup may handle
 *   its own panic; an escaping cleanup panic fails compilation without abort.
 */
static XrXirCtfeStatus ctfe_panic_status(uint32_t code) {
    switch (code) {
    case XR_XIR_PANIC_DIVIDE: case XR_XIR_PANIC_REMAINDER: return XR_XIR_CTFE_DIVIDE_BY_ZERO;
    case XR_XIR_PANIC_RANGE: return XR_XIR_CTFE_NUMERIC_RANGE;
    case XR_XIR_PANIC_NULL_UNWRAP: return XR_XIR_CTFE_NULL_UNWRAP;
    case XR_XIR_PANIC_BOUNDS: return XR_XIR_CTFE_BOUNDS;
    case XR_XIR_PANIC_ASSERTION: return XR_XIR_CTFE_ASSERTION_FAILURE;
    case XR_XIR_PANIC_MATCH: return XR_XIR_CTFE_MATCH_FAILURE;
    default: return XR_XIR_CTFE_BAD_ARTIFACT;
    }
}
static XrXirCtfeStatus ctfe_panic_begin(CtfeCallExecution *execution,const XrXirPanicPayload *panic,uint32_t at) {
    CtfeCallFrame *frame=execution->top;const XrXirFunction *body=frame->run.function;
    if (at>=body->instruction_count || frame->exit_kind || !xr_xir_panic_empty(&frame->panic)) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t low=0,high=body->block_count;
    while (low+1<high) {
        if (!scalar_compile_work(&frame->run,2)) return XR_XIR_CTFE_BUDGET;
        uint32_t middle=low+(high-low)/2;if (body->blocks[middle].first<=at) low=middle;else high=middle;
    }
    if (!scalar_compile_work(&frame->run,2*sizeof(XrXirPanicPayload)+32)) return XR_XIR_CTFE_BUDGET;
    XrXirCtfeStatus status=ctfe_value_status(xr_xir_panic_copy(panic,&frame->panic));if (status!=XR_XIR_CTFE_OK) return status;
    uint32_t handler=body->blocks[low].panic;
    frame->state.landing=handler;frame->state.exit_target=handler?body->blocks[handler].frontier:0;
    frame->exit_kind=CTFE_EXIT_PANIC;frame->state.leaving=true;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_panic_fault(CtfeCallExecution *execution,XrXirCtfeStatus status,uint32_t at) {
    uint32_t code=0;CtfeCallFrame *frame=execution->top;
    const XrXirInstruction *op=&frame->run.function->instructions[at];
    switch (status) {
    case XR_XIR_CTFE_DIVIDE_BY_ZERO: code=op->op==XR_XIR_REM_INT?XR_XIR_PANIC_REMAINDER:XR_XIR_PANIC_DIVIDE;break;
    case XR_XIR_CTFE_NUMERIC_RANGE: code=XR_XIR_PANIC_RANGE;break;
    case XR_XIR_CTFE_NULL_UNWRAP: code=XR_XIR_PANIC_NULL_UNWRAP;break;
    case XR_XIR_CTFE_BOUNDS: code=XR_XIR_PANIC_BOUNDS;break;
    case XR_XIR_CTFE_ASSERTION_FAILURE: code=XR_XIR_PANIC_ASSERTION;break;
    case XR_XIR_CTFE_MATCH_FAILURE: code=XR_XIR_PANIC_MATCH;break;
    default: return status;
    }
    XrXirPanicPayload panic={{code,0,0,0},{0}};
    if (code==XR_XIR_PANIC_BOUNDS) {
        if (!xr_xir_fault_bounds_valid(execution->owned.fault)) return XR_XIR_CTFE_BAD_ARTIFACT;
        panic.detail=execution->owned.fault;
    }
    if (code==XR_XIR_PANIC_ASSERTION) panic.message=ctfe_nominal_operand(&frame->run,op->args[1]);
    return ctfe_panic_begin(execution,&panic,at);
}
static XrXirCtfeStatus ctfe_panic_land(CtfeCallExecution *execution) {
    CtfeCallFrame *frame=execution->top;const XrXirFunction *body=frame->run.function;
    uint32_t first=body->blocks[frame->state.landing].first;
    const XrXirInstruction *marker=&body->instructions[first];
    if (marker->op!=XR_XIR_PANIC_CATCH) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (marker->type==XR_XIR_PANIC_INFO) {
        if (ctfe_frame_spare(&execution->owned)<512) return XR_XIR_CTFE_FRAME_LIMIT;
        if (!scalar_compile_work(&frame->run,1024)) return XR_XIR_CTFE_BUDGET;
        XrXirValue info={0};XrXirCtfeStatus status=ctfe_value_status(xr_xir_panic_info_new(execution->owned.domain,&frame->panic,&info));
        if (status!=XR_XIR_CTFE_OK) return status;
        xr_xir_owned_slot_move(frame->run.frame,frame->run.layout->offsets[body->parameter_count+first],&info);
    } else if (marker->type!=XR_XIR_UNIT) return XR_XIR_CTFE_BAD_ARTIFACT;
    xr_xir_panic_drop(&frame->panic);frame->exit_kind=CTFE_EXIT_NONE;
    frame->state.landing=0;frame->state.leaving=false;frame->state.instruction=first+1;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_panic_continue(CtfeCallExecution *execution,bool *done) {
    CtfeCallFrame *frame=execution->top,*parent=frame->parent;
    if (frame->state.landing) return ctfe_panic_land(execution);
    if (frame->cleanup) return XR_XIR_CTFE_CLEANUP_PANIC;
    if (!parent) { *done=true;return ctfe_panic_status(frame->panic.detail.code); }
    if (frame->return_value<parent->run.function->parameter_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t at=frame->return_value-parent->run.function->parameter_count;
    if (!scalar_compile_work(&frame->run,2*sizeof(XrXirPanicPayload)+64)) return XR_XIR_CTFE_BUDGET;
    XrXirPanicPayload held={0};xr_xir_panic_move(&frame->panic,&held);
    execution->top=parent;--execution->depth;execution->live_bytes-=frame->bytes;ctfe_call_frame_free(frame);
    execution->owned.activation=parent;execution->owned.authority=(XrXirCellAuthority){execution,parent,parent->epoch};
    execution->owned.frame_spare=execution->request->evaluation.limits.frame_bytes-execution->live_bytes-execution->loan_bytes;
    execution->owned.loan_base=execution->loan_bytes;
    XrXirCtfeStatus status=ctfe_panic_begin(execution,&held,at);xr_xir_panic_drop(&held);return status;
}
