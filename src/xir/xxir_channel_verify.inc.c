/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_verify.inc.c - Exact Channel receivers and governed message results
 */
static XrXirStatus channel_uses(const Graph *graph,const XrXirFunction *function,
    VerifyContext *context,uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];const XrXirTypes *types=context->module->types;
    bool timed=op->op==XR_XIR_CHANNEL_SEND_TIMEOUT || op->op==XR_XIR_CHANNEL_RECV_TIMEOUT;
    bool send=op->op==XR_XIR_CHANNEL_TRY_SEND || op->op==XR_XIR_CHANNEL_SEND ||
        op->op==XR_XIR_CHANNEL_SEND_TIMEOUT,create=op->op==XR_XIR_CHANNEL_NEW;
    bool fallback=op->op==XR_XIR_CHANNEL_RECV_OR,group=send || fallback || timed;
    if (group && !op->args[1]) return XR_XIR_BAD_STRUCTURE;
    uint32_t receiver_id=group ? function->operands[op->args[0]] : op->args[0];
    if (!create && receiver_id>=(uint64_t)function->parameter_count+function->instruction_count) return XR_XIR_BAD_VALUE;
    XrXirType receiver=create ? op->type : xr_xir_operand_type(function,receiver_id);
    if (!xr_xir_type_is_channel(types,receiver)) return XR_XIR_BAD_TYPE;
    XrXirStatus status=type_use_context(context,context->location.function,receiver);
    if (status!=XR_XIR_OK) return status;
    XrXirType element=xr_xir_channel_element(types,receiver);
    if (op->op==XR_XIR_CHANNEL_TRY_SEND || op->op==XR_XIR_CHANNEL_TRY_RECV || op->op==XR_XIR_CHANNEL_RECV || timed) {

        if ((op->op==XR_XIR_CHANNEL_TRY_SEND || op->op==XR_XIR_CHANNEL_SEND_TIMEOUT) ? !xr_xir_nominal_native_send_result(types,op->type) : !xr_xir_nominal_native_recv(types,op->type,element))
            return XR_XIR_BAD_TYPE;
        status=type_use_context(context,context->location.function,op->type);
    }
    uint32_t message=(send || fallback) && element!=XR_XIR_UNIT ? 1u : 0u;
    if (group && op->args[1]!=1u+message+(timed ? 1u : 0u)) return XR_XIR_BAD_STRUCTURE;
    if (fallback && op->type!=element) return XR_XIR_BAD_TYPE;
    if (status==XR_XIR_OK) status=role_operand(function,graph,context,instruction,0,create ? XR_XIR_I64 : receiver);
    if (status==XR_XIR_OK && message) status=role_operand(function,graph,context,instruction,1,element);
    if (status==XR_XIR_OK && timed) status=role_operand(function,graph,context,instruction,1u+message,XR_XIR_I64);
    return status;
}
