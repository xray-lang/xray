/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_select.inc.c - Resume the same instruction around a joint wait
 *
 * KEY CONCEPT:
 *   Outgoing words are borrowed mirrors. The Call owns the selection while
 *   parked, and a committed tuple enters its owned slot without another check.
 */
static XrXirRunStatus vm_select_step(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination,XrXirAction *action) {
    uint32_t count=op->args[1]-(op->immediate==2?1u:0u);
    for(uint32_t a=0;a<count;++a)
        state->arguments[a]=vm_value_operand(run,run->function->operands[op->args[0]+a]);
    XrXirValue output={0};
    XrXirCallStatus status=xr_xir_instance_channel_select_at(run->view,state->instruction,
        run->frame,&state->select,state->arguments,&output);
    if(status==XR_XIR_CALL_BUSY||status==XR_XIR_CALL_CANCELLED||status==XR_XIR_CALL_SUSPENDED||status==XR_XIR_CALL_DEFER_ASYNC) {
        *action=(XrXirAction){.kind=status==XR_XIR_CALL_SUSPENDED||status==XR_XIR_CALL_DEFER_ASYNC?
            XR_XIR_ACTION_AWAIT_CHANNEL:XR_XIR_ACTION_CONTINUE};return XR_XIR_RUN_OK;
    }
    if(status!=XR_XIR_CALL_READY)return status==XR_XIR_CALL_OOM?XR_XIR_RUN_OUT_OF_MEMORY:
        status==XR_XIR_CALL_LIMIT?XR_XIR_RUN_FRAME_LIMIT:status==XR_XIR_CALL_HOST_ERROR?XR_XIR_RUN_HOST_ERROR:
        status==XR_XIR_CALL_NUMERIC_RANGE?XR_XIR_RUN_NUMERIC_RANGE:
        status==XR_XIR_CALL_UNSUPPORTED?XR_XIR_RUN_UNSUPPORTED:XR_XIR_RUN_BAD_ARTIFACT;
    xr_xir_owned_slot_move(run->frame,destination,&output);++state->instruction;return XR_XIR_RUN_OK;
}
