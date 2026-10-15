/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_cleanup.inc.c - Static lexical frontier execution on compiler frames
 *
 * KEY CONCEPT:
 *   A registered cleanup retires only after its real child returns. All edges
 *   use the common predecessor-aware kernel; pending returns retain snapshots.
 */
static XrXirCtfeStatus ctfe_cleanup_edge(CtfeCallExecution *execution,uint32_t target) {
    CtfeCallFrame *frame=execution->top;const XrXirFunction *body=frame->run.function;
    if (target>=body->block_count) return XR_XIR_CTFE_BAD_ARTIFACT;
    if (!scalar_compile_work(&frame->run,16)) return XR_XIR_CTFE_BUDGET;
    XrXirInstruction edge={.op=XR_XIR_JUMP,.targets={target}};
    XrXirCtfeStatus status=ctfe_tuple_flow_work(&frame->run,&edge,&execution->owned);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirRunStatus moved=xir_scalar_core_edge(&frame->run,frame->state.instruction,target);
    if (moved!=XR_XIR_RUN_OK) return moved==XR_XIR_RUN_STEP_LIMIT?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_BAD_ARTIFACT;
    frame->state.instruction=body->blocks[target].first;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_cleanup_step(CtfeCallExecution *execution,const XrXirInstruction *op) {
    CtfeCallFrame *frame=execution->top;uint32_t registration=frame->state.instruction+1;
    if (op->op==XR_XIR_CLEANUP_REGISTER || frame->state.frontier==(uint32_t)op->immediate) {
        XrXirCtfeStatus status=ctfe_cleanup_edge(execution,op->targets[0]);
        if (status==XR_XIR_CTFE_OK && op->op==XR_XIR_CLEANUP_REGISTER) frame->state.frontier=registration;
        return status;
    }
    if (!scalar_compile_work(&frame->run,3*sizeof(uint32_t)+sizeof(bool))) return XR_XIR_CTFE_BUDGET;
    frame->state.exit_target=(uint32_t)op->immediate;frame->state.exit_block=op->targets[0];frame->state.leaving=true;
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_cleanup_continue(CtfeCallExecution *execution,XrXirValue *output,bool *done) {
    CtfeCallFrame *frame=execution->top;const XrXirFunction *body=frame->run.function;
    if (!scalar_compile_work(&frame->run,64)) return XR_XIR_CTFE_BUDGET;
    if (frame->state.frontier==frame->state.exit_target) {
        if (frame->returning) {
            *done=!frame->parent;return ctfe_call_return(execution,&frame->returned,output);
        }
        frame->state.leaving=false;return ctfe_cleanup_edge(execution,frame->state.exit_block);
    }
    if (!frame->state.frontier || frame->state.frontier>body->instruction_count || frame->state.cleanup_waiting)
        return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t at=frame->state.frontier-1,low=0,high=body->block_count;
    while (low+1<high) {
        if (!scalar_compile_work(&frame->run,2)) return XR_XIR_CTFE_BUDGET;
        uint32_t middle=low+(high-low)/2;if (body->blocks[middle].first<=at) low=middle;else high=middle;
    }
    const XrXirInstruction *registration=&body->instructions[at];
    if (registration->op!=XR_XIR_CLEANUP_REGISTER) return XR_XIR_CTFE_BAD_ARTIFACT;
    uint32_t parent=body->blocks[low].frontier;frame->state.cleanup_waiting=true;
    XrXirCtfeStatus status=ctfe_call_enter(execution,registration,true);
    if (status==XR_XIR_CTFE_OK) execution->top->cleanup_parent=parent;
    return status;
}
