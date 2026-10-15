/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_tuple_verify.inc.c - Typed ordered fields and authenticated zero-payload Unit values
 */
static XrXirStatus tuple_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    bool construct=op->op==XR_XIR_TUPLE_NEW;
    XrXirType type=construct ? op->type : xr_xir_operand_type(function,op->args[0]);
    XrXirTypeDescription description={0};
    XrXirStatus status=xr_xir_compile_type_description(&context->remaining,context->module->types,type,&description);
    if (status!=XR_XIR_OK) return status;
    if (construct && type==XR_XIR_UNIT) {
        status=type_use_context(context,context->location.function,type);
        return status==XR_XIR_OK && (op->args[0] || op->args[1]) ? XR_XIR_BAD_STRUCTURE : status;
    }
    if (description.kind!=XR_XIR_TYPE_TUPLE) return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node=description.node;
    status=type_use_context(context,context->location.function,type);
    if (status!=XR_XIR_OK) return status;
    if (!construct) {
        if (op->immediate<0 || (uint64_t)op->immediate>=node->parameter_count) return XR_XIR_BAD_STRUCTURE;
        XrXirFieldDescription member={0};
        status=xr_xir_compile_description_field(&context->remaining,&description,(uint32_t)op->immediate,&member);
        if (status!=XR_XIR_OK) return status;
        if (op->type!=member.type) return XR_XIR_BAD_TYPE;
        return role_operand(function,graph,context,instruction,0,type);
    }
    if (!xir_compile_work(&context->remaining,node->parameter_count)) return XR_XIR_BUDGET;
    uint32_t payloads=0;
    for (uint32_t p=0; p<node->parameter_count; ++p) {
        XrXirFieldDescription member={0};
        status=xr_xir_compile_description_field(&context->remaining,&description,p,&member);
        if (status!=XR_XIR_OK) return status;
        XrXirType field=member.type;
        if (field==XR_XIR_UNIT) continue;
        if (payloads>=op->args[1]) return XR_XIR_BAD_STRUCTURE;
        status=role_operand(function,graph,context,instruction,payloads,field);
        if (status!=XR_XIR_OK) return status;
        ++payloads;
    }
    return payloads==op->args[1] ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
