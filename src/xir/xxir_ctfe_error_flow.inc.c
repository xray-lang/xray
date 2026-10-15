/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_error_flow.inc.c - Explicit error continuations on compiler frames
 *
 * KEY CONCEPT:
 *   An error snapshot outlives the throwing child and its cleanup. Parent
 *   authority is restored before delivering the authentic retained enum.
 */
enum { CTFE_EXIT_NONE,CTFE_EXIT_RETURN,CTFE_EXIT_ERROR,CTFE_EXIT_LOCAL_ERROR };
static XrXirCtfeStatus ctfe_frame_edge(CtfeCallExecution *execution,CtfeCallFrame *frame,uint32_t target) {
    const XrXirFunction *body=frame->run.function;
    if (target>=body->block_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!scalar_compile_work(&frame->run,16)) return XR_XIR_CTFE_BUDGET;
    XrXirInstruction edge={.op=XR_XIR_JUMP,.targets={target}};
    XrXirCtfeStatus status=ctfe_tuple_flow_work(&frame->run,&edge,&execution->owned);if (status!=XR_XIR_CTFE_OK) return status;
    XrXirRunStatus moved=xir_scalar_core_edge(&frame->run,frame->state.instruction,target);
    if (moved!=XR_XIR_RUN_OK) return moved==XR_XIR_RUN_STEP_LIMIT?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_BAD_ARTIFACT;
    frame->state.instruction=body->blocks[target].first;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_invoke_deliver(CtfeCallExecution *execution,CtfeCallFrame *parent,
    const XrXirValue *value,bool error) {
    const XrXirFunction *body=parent->run.function;uint32_t at=parent->state.instruction;
    const XrXirInstruction *call=&body->instructions[at];
    if (call->op!=XR_XIR_INVOKE && call->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t target=call->targets[error?1:0],first=body->blocks[target].first;
    const XrXirInstruction *marker=&body->instructions[first];XrXirType expected=error?XR_XIR_ERROR:call->type;
    bool discard=!error && marker->op==XR_XIR_INVOKE_DISCARD;
    bool produced=error || marker->op==XR_XIR_INVOKE_RESULT || discard;
    if ((error && marker->op!=XR_XIR_INVOKE_ERROR) || (!error && expected!=XR_XIR_UNIT && !produced) ||
        (produced && marker->immediate!=(int64_t)at) || (!discard && produced && marker->type!=expected)) return XR_XIR_CTFE_BAD_ARTIFACT;
    CtfeOwnedState *state=&execution->owned;XrXirValue erased={0};const XrXirValue *received=value;
    XrXirCtfeStatus status=ctfe_validation_work(&parent->run,state,expected);
    if (status==XR_XIR_CTFE_OK && error) {
        status=ctfe_admission_work(&parent->run,state,value);
        if (status==XR_XIR_CTFE_OK) {
            XrXirValueAdmission admission=ctfe_value_admission(state);
            status=ctfe_value_status(xr_xir_error_erase(value,&admission,&erased));received=&erased;
        }
    }
    if (status==XR_XIR_CTFE_OK && !ctfe_result_value(received,expected,state->arena)) status=XR_XIR_CTFE_BAD_ARTIFACT;
    if (status==XR_XIR_CTFE_OK) status=ctfe_frame_edge(execution,parent,target);
    if (status==XR_XIR_CTFE_OK && !discard && expected!=XR_XIR_UNIT)
        status=ctfe_argument_store(&parent->run,parent->run.layout->offsets[body->parameter_count+first],received,state,false);
    if (status==XR_XIR_CTFE_OK && produced) ++parent->state.instruction;
    xr_xir_value_drop(&erased);return status;
}
static XrXirCtfeStatus ctfe_error_begin(CtfeCallExecution *execution,const XrXirValue *value,uint8_t kind) {
    CtfeCallFrame *frame=execution->top;XrXirValue concrete={0};
    XrXirCtfeStatus status=ctfe_error_concrete(&frame->run,&execution->owned,value,&concrete);
    if (status==XR_XIR_CTFE_OK) status=ctfe_admission_work(&frame->run,&execution->owned,&concrete);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (!scalar_compile_work(&frame->run,sizeof(XrXirValue)+16)) return XR_XIR_CTFE_BUDGET;
    status=ctfe_value_status(xr_xir_value_copy(&concrete,&frame->returned));if (status!=XR_XIR_CTFE_OK) return status;
    frame->exit_kind=kind;frame->state.leaving=true;
    const XrXirInstruction *op=&frame->run.function->instructions[frame->state.instruction];
    frame->state.exit_target=kind==CTFE_EXIT_ERROR?0:(uint32_t)op->immediate;frame->state.exit_block=op->targets[0];
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_error_unwind(CtfeCallExecution *execution,bool *done) {
    CtfeCallFrame *frame=execution->top,*parent=frame->parent;
    if (!parent) { *done=true;return XR_XIR_CTFE_UNCAUGHT_ERROR; }
    if (frame->cleanup || parent->exit_kind) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!scalar_compile_work(&frame->run,2*sizeof(XrXirValue)+64)) return XR_XIR_CTFE_BUDGET;
    bool invoked=frame->invoked;XrXirValue held=frame->returned;frame->returned=(XrXirValue){0};
    execution->top=parent;--execution->depth;execution->live_bytes-=frame->bytes;ctfe_call_frame_free(frame);
    execution->owned.activation=parent;execution->owned.authority=(XrXirCellAuthority){execution,parent,parent->epoch};
    execution->owned.frame_spare=execution->request->evaluation.limits.frame_bytes-execution->live_bytes;
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    if (invoked) status=ctfe_invoke_deliver(execution,parent,&held,true);
    else {
        parent->returned=held;held=(XrXirValue){0};parent->exit_kind=CTFE_EXIT_ERROR;
        parent->state.leaving=true;parent->state.exit_target=0;
    }
    xr_xir_value_drop(&held);return status;
}
