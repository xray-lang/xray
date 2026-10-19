/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_tuple_verify.inc.c - Ordered fields with a strict non-Unit operand table
 */
static XrXirStatus tuple_uses(const Graph *graph, const XrXirFunction *function,
    VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    bool construct=op->op==XR_XIR_TUPLE_NEW;
    XrXirType type=construct ? op->type : xr_xir_operand_type(function,op->args[0]);
    const XrXirTypeNode *node=xr_xir_tuple_signature(context->module->types,type);
    if (!node) return XR_XIR_BAD_TYPE;
    XrXirStatus status=type_use_context(context,context->location.function,type);
    if (status!=XR_XIR_OK) return status;
    if (!construct) {
        if (op->immediate<0 || (uint64_t)op->immediate>=node->parameter_count) return XR_XIR_BAD_STRUCTURE;
        if (op->type!=node->parameters[op->immediate].type) return XR_XIR_BAD_TYPE;
        return role_operand(function,graph,context,instruction,0,type);
    }
    if (!xir_compile_work(&context->remaining,node->parameter_count)) return XR_XIR_BUDGET;
    uint32_t payloads=0;
    for (uint32_t p=0; p<node->parameter_count; ++p) {
        XrXirType field=node->parameters[p].type;
        if (field==XR_XIR_UNIT) continue;
        if (payloads>=op->args[1]) return XR_XIR_BAD_STRUCTURE;
        status=role_operand(function,graph,context,instruction,payloads,field);
        if (status!=XR_XIR_OK) return status;
        ++payloads;
    }
    return payloads==op->args[1] ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}
