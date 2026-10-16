/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_channel.inc.c - Channel identity execution under active call authority
 */
static XrXirRunStatus vm_channel_step(ScalarRun *run,VmState *state,
    const XrXirInstruction *op,uint32_t destination,XrXirAction *action) {
    XrXirValueAdmission *admission=xr_xir_call_admission(run->view);
    if (!admission) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirCallStatus permission=xr_xir_call_execution_status(run->view);
    if (permission==XR_XIR_CALL_CANCELLED) return XR_XIR_RUN_OK;
    if (permission!=XR_XIR_CALL_READY) return XR_XIR_RUN_BAD_ARTIFACT;
    XrXirValue output={0};XrXirValueStatus status=XR_XIR_VALUE_BAD_ARGUMENT;
    if (op->op==XR_XIR_CHANNEL_SEND || op->op==XR_XIR_CHANNEL_RECV || op->op==XR_XIR_CHANNEL_RECV_OR ||
        op->op==XR_XIR_CHANNEL_SEND_TIMEOUT || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT) {
        bool timed=op->op==XR_XIR_CHANNEL_SEND_TIMEOUT || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT;
        bool receive=op->op==XR_XIR_CHANNEL_RECV || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT;
        uint32_t receiver_id=receive && !timed ? op->args[0] : run->function->operands[op->args[0]];
        XrXirValue receiver=vm_value_operand(run,receiver_id),argument={0};
        if (!receive && op->args[1]==(timed ? 3u : 2u)) argument=vm_value_operand(run,run->function->operands[op->args[0]+1]);
        XrXirChannelCallOperation operation=timed ? (receive ? XR_XIR_CHANNEL_CALL_RECV_TIMEOUT : XR_XIR_CHANNEL_CALL_SEND_TIMEOUT) :
            op->op==XR_XIR_CHANNEL_SEND ? XR_XIR_CHANNEL_CALL_SEND :
            receive ? XR_XIR_CHANNEL_CALL_RECV : XR_XIR_CHANNEL_CALL_RECV_OR;
        int64_t milliseconds=timed ? (int64_t)vm_value_operand(run,run->function->operands[op->args[0]+op->args[1]-1]).payload : 0;
        const XrXirChannelCallRequest request={operation,&receiver,receive ? NULL : &argument,op->type,milliseconds};
        XrXirCallStatus waiting=xr_xir_call_channel_step(run->view,&state->channel,&request,&output);
        if (waiting==XR_XIR_CALL_SUSPENDED || waiting==XR_XIR_CALL_DEFER_ASYNC || waiting==XR_XIR_CALL_BUSY || waiting==XR_XIR_CALL_CANCELLED) {
            *action=(XrXirAction){.kind=(waiting==XR_XIR_CALL_SUSPENDED || waiting==XR_XIR_CALL_DEFER_ASYNC) ? XR_XIR_ACTION_AWAIT_CHANNEL : XR_XIR_ACTION_CONTINUE};return XR_XIR_RUN_OK;
        }
        if (waiting!=XR_XIR_CALL_READY) return waiting==XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY :
            waiting==XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT :
            waiting==XR_XIR_CALL_HOST_ERROR ? XR_XIR_RUN_HOST_ERROR :
            waiting==XR_XIR_CALL_NUMERIC_RANGE ? XR_XIR_RUN_NUMERIC_RANGE :
            waiting==XR_XIR_CALL_UNSUPPORTED ? XR_XIR_RUN_UNSUPPORTED : XR_XIR_RUN_BAD_ARTIFACT;
        status=XR_XIR_VALUE_OK;
    } else if (op->op==XR_XIR_CHANNEL_NEW)
        status=xr_xir_channel_new(op->type,vm_value_operand(run,op->args[0]).payload,admission,&output);
    else if (op->op==XR_XIR_CHANNEL_TRY_SEND || op->op==XR_XIR_CHANNEL_TRY_RECV) {
        bool send=op->op==XR_XIR_CHANNEL_TRY_SEND;
        uint32_t receiver_id=send ? run->function->operands[op->args[0]] : op->args[0];
        XrXirValue receiver=vm_value_operand(run,receiver_id),message={0};XrXirChannelState outcome=XR_XIR_CHANNEL_RETRY;
        if (send && op->args[1]==2) message=vm_value_operand(run,run->function->operands[op->args[0]+1]);
        /* Owned SSA operands remain in this frame across a scheduling retry. */
        status=send ? xr_xir_channel_try_send(&receiver,&message,op->type,admission,&output,&outcome) :
            xr_xir_channel_receive_result(&receiver,op->type,admission,&output,&outcome);
        if (status==XR_XIR_VALUE_OK && outcome==XR_XIR_CHANNEL_RETRY) {
            *action=(XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{0},{0},0};return XR_XIR_RUN_OK;
        }
    } else {
        XrXirValue receiver=vm_value_operand(run,op->args[0]);
        if (op->op==XR_XIR_CHANNEL_CLOSE) {
            XrXirChannelState outcome=XR_XIR_CHANNEL_RETRY;
            status=xr_xir_channel_close(&receiver,admission,&outcome);
            if (status==XR_XIR_VALUE_OK && outcome==XR_XIR_CHANNEL_RETRY) {
                *action=(XrXirAction){XR_XIR_ACTION_CONTINUE,0,NULL,0,{0},{0},0};return XR_XIR_RUN_OK;
            }
        } else {
            int64_t capacity=0;bool closed=false;
            status=xr_xir_channel_info(&receiver,admission,&capacity,&closed);
            output=(XrXirValue){op->op==XR_XIR_CHANNEL_CAPACITY ? XR_XIR_I64 : XR_XIR_BOOL,0,
                op->op==XR_XIR_CHANNEL_CAPACITY ? capacity : closed};
        }
    }
    if (status==XR_XIR_VALUE_BOUNDS) return XR_XIR_RUN_NUMERIC_RANGE;
    if (status!=XR_XIR_VALUE_OK) return value_run_status(status);
    permission=xr_xir_call_execution_status(run->view);
    if (permission!=XR_XIR_CALL_READY) {
        xr_xir_value_drop(&output);return permission==XR_XIR_CALL_CANCELLED ? XR_XIR_RUN_OK : XR_XIR_RUN_BAD_ARTIFACT;
    }
    if (xr_xir_type_is_owned(run->module->types,op->type)) xr_xir_owned_slot_move(run->frame,destination,&output);
    else if (op->type!=XR_XIR_UNIT) xr_xir_scalar_store(run->frame,destination,output.payload);
    ++state->instruction;return XR_XIR_RUN_OK;
}
