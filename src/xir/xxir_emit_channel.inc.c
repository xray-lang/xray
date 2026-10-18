/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_channel.inc.c - Portable Channel identity execution statements
 */
static void emit_channel_try(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout) {
    bool send=op->op==XR_XIR_CHANNEL_TRY_SEND;
    uint32_t receiver=send ? function->operands[op->args[0]] : op->args[0];
    append(buffer,"        XrXirValue receiver = ");emit_value(buffer,function,layout,receiver);append(buffer,";\n");
    if (send) {
        append(buffer,"        XrXirValue message = ");
        if (op->args[1]==2) emit_value(buffer,function,layout,function->operands[op->args[0]+1]);
        else append(buffer,"(XrXirValue){0}");
        append(buffer,";\n");
    }
    append(buffer,"        XrXirChannelState outcome = XR_XIR_CHANNEL_RETRY;\n");
    if (send) append(buffer,"        status = xr_xir_channel_try_send(&receiver, &message, (XrXirType)%uu, "
        "xr_xir_call_admission(view), &value, &outcome);\n",(uint32_t)op->type);
    else append(buffer,"        status = xr_xir_channel_receive_result(&receiver, (XrXirType)%uu, "
        "xr_xir_call_admission(view), &value, &outcome);\n",(uint32_t)op->type);
    append(buffer,"        if (status == XR_XIR_VALUE_OK && outcome == XR_XIR_CHANNEL_RETRY) return (XrXirAction)"
        "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
}
static void emit_channel_blocking(CBuffer *buffer,const XrXirFunction *function,const XrXirInstruction *op,
    const XrXirFunctionLayout *layout,uint32_t index) {
    bool timed=op->op==XR_XIR_CHANNEL_SEND_TIMEOUT || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT;
    bool receive=op->op==XR_XIR_CHANNEL_RECV || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT;
    uint32_t receiver=receive && !timed ? op->args[0] : function->operands[op->args[0]];
    append(buffer,"        XrXirValue receiver = ");emit_value(buffer,function,layout,receiver);append(buffer,";\n");
    append(buffer,"        XrXirValue argument = ");
    if (!receive && op->args[1]==(timed ? 3u : 2u)) emit_value(buffer,function,layout,function->operands[op->args[0]+1]);
    else append(buffer,"(XrXirValue){0}");
    append(buffer,";\n        const int64_t milliseconds = ");
    if (timed) {
        append(buffer,"(int64_t) xr_xir_scalar_load(state->frame, %uu)",
            layout->offsets[function->operands[op->args[0]+op->args[1]-1]]);
    } else append(buffer,"0");
    append(buffer,";\n        const XrXirChannelCallRequest request = {%s, &receiver, %s, (XrXirType)%uu, milliseconds};\n"
        "        XrXirCallStatus waiting = xr_xir_call_channel_step(view, &state->channel, &request, &value);\n",
        timed ? (receive ? "XR_XIR_CHANNEL_CALL_RECV_TIMEOUT" : "XR_XIR_CHANNEL_CALL_SEND_TIMEOUT") :
        op->op==XR_XIR_CHANNEL_SEND ? "XR_XIR_CHANNEL_CALL_SEND" : receive ? "XR_XIR_CHANNEL_CALL_RECV" : "XR_XIR_CHANNEL_CALL_RECV_OR",
        receive ? "NULL" : "&argument",(uint32_t)op->type);
    append(buffer,"        (void) argument;\n"
        "        if (waiting == XR_XIR_CALL_SUSPENDED || waiting == XR_XIR_CALL_DEFER_ASYNC || waiting == XR_XIR_CALL_BUSY || waiting == XR_XIR_CALL_CANCELLED)\n"
        "            return (XrXirAction){.kind = (waiting == XR_XIR_CALL_SUSPENDED || waiting == XR_XIR_CALL_DEFER_ASYNC) ? XR_XIR_ACTION_AWAIT_CHANNEL : XR_XIR_ACTION_CONTINUE};\n"
        "        if (waiting != XR_XIR_CALL_READY) ");
    emit_fault_return(buffer,function,layout,index,
        "xr_xir_call_fault(waiting == XR_XIR_CALL_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : "
        "waiting == XR_XIR_CALL_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : "
        "waiting == XR_XIR_CALL_HOST_ERROR ? XR_XIR_RUN_HOST_ERROR : "
        "waiting == XR_XIR_CALL_NUMERIC_RANGE ? XR_XIR_RUN_NUMERIC_RANGE : "
        "waiting == XR_XIR_CALL_UNSUPPORTED ? XR_XIR_RUN_UNSUPPORTED : XR_XIR_RUN_BAD_ARTIFACT)");
    append(buffer,"\n        status = XR_XIR_VALUE_OK;\n");
}
static void emit_channel_step(CBuffer *buffer,const XrXirFunction *function,
    const XrXirInstruction *op,const XrXirFunctionLayout *layout,uint32_t destination,uint32_t index) {
    append(buffer,"        state->pc = %uu;\n        { XrXirValue value = {0};\n"
        "        XrXirCallStatus permission = xr_xir_call_execution_status(view);\n"
        "        if (permission == XR_XIR_CALL_CANCELLED) return (XrXirAction)"
        "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n"
        "        if (permission != XR_XIR_CALL_READY) return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT);\n"
        "        XrXirValueStatus status;\n",index);
    if (op->op==XR_XIR_CHANNEL_SEND || op->op==XR_XIR_CHANNEL_RECV || op->op==XR_XIR_CHANNEL_RECV_OR ||
        op->op==XR_XIR_CHANNEL_SEND_TIMEOUT || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT) {
        emit_channel_blocking(buffer,function,op,layout,index);
    } else if (op->op==XR_XIR_CHANNEL_NEW) {
        append(buffer,"        status = xr_xir_channel_new((XrXirType)%uu, xr_xir_scalar_load(state->frame, %uu), "
            "xr_xir_call_admission(view), &value);\n",(uint32_t)op->type,layout->offsets[op->args[0]]);
    } else if (op->op==XR_XIR_CHANNEL_TRY_SEND || op->op==XR_XIR_CHANNEL_TRY_RECV) {
        emit_channel_try(buffer,function,op,layout);
    } else {
        append(buffer,"        XrXirValue receiver = ");emit_value(buffer,function,layout,op->args[0]);append(buffer,";\n");
        if (op->op==XR_XIR_CHANNEL_CLOSE) {
            append(buffer,"        XrXirChannelState outcome = XR_XIR_CHANNEL_RETRY;\n"
                "        status = xr_xir_channel_close(&receiver, xr_xir_call_admission(view), &outcome);\n"
                "        if (status == XR_XIR_VALUE_OK && outcome == XR_XIR_CHANNEL_RETRY) return (XrXirAction)"
                "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n");
        } else {
            append(buffer,"        int64_t capacity = 0; bool closed = false;\n"
                "        status = xr_xir_channel_info(&receiver, xr_xir_call_admission(view), &capacity, &closed);\n"
                "        value = (XrXirValue){%uu, 0, %s};\n",(uint32_t)op->type,
                op->op==XR_XIR_CHANNEL_CAPACITY ? "capacity" : "closed");
        }
    }
    append(buffer,"        if (status != XR_XIR_VALUE_OK) ");
    emit_fault_return(buffer,function,layout,index,
        "xr_xir_call_fault(status == XR_XIR_VALUE_BOUNDS ? XR_XIR_RUN_NUMERIC_RANGE : "
        "status == XR_XIR_VALUE_OOM ? XR_XIR_RUN_OUT_OF_MEMORY : "
        "status == XR_XIR_VALUE_LIMIT || status == XR_XIR_VALUE_REFCOUNT_LIMIT ? XR_XIR_RUN_FRAME_LIMIT : "
        "status == XR_XIR_VALUE_UNSUPPORTED ? XR_XIR_RUN_UNSUPPORTED : XR_XIR_RUN_BAD_ARTIFACT)");
    append(buffer,"\n        permission = xr_xir_call_execution_status(view);\n"
        "        if (permission != XR_XIR_CALL_READY) { xr_xir_value_drop(&value);\n"
        "            if (permission == XR_XIR_CALL_CANCELLED) return (XrXirAction)"
        "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n"
        "            return xr_xir_call_fault(XR_XIR_RUN_BAD_ARTIFACT); }\n");
    if (xr_xir_type_is_owned(buffer->types,op->type)) append(buffer,"        xr_xir_owned_slot_move(state->frame, %uu, &value);\n",destination);
    else if (op->type!=XR_XIR_UNIT) append(buffer,"        xr_xir_scalar_store(state->frame, %uu, value.payload);\n",destination);
    append(buffer,"        state->pc = %uu;\n        }\n        return (XrXirAction)"
        "{XR_XIR_ACTION_CONTINUE, 0, NULL, 0, {0}, {0}, 0};\n",index+1);
}
