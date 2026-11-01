/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_fixed_array_verify.inc.c - Exact typed byte construction and observations
 *
 * KEY CONCEPT:
 *   Length metadata and every real u8 operand are independently checked.
 */
static XrXirStatus fixed_array_uses(const Graph *graph,const XrXirFunction *function,
    VerifyContext *context,uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    bool construct=op->op==XR_XIR_FIXED_ARRAY_NEW;
    XrXirType type=construct?op->type:xr_xir_operand_type(function,op->args[0]);
    const XrXirTypes *types=context->module->types;
    uint32_t length=xr_xir_fixed_array_length(types,type);
    if(!length)return XR_XIR_BAD_TYPE;
    XrXirStatus status=type_use_context(context,context->location.function,type);
    if(status!=XR_XIR_OK)return status;
    if(construct) {
        if(op->args[1]!=length)return XR_XIR_BAD_STRUCTURE;
        for(uint32_t p=0;p<length;++p) {
            status=role_operand(function,graph,context,instruction,p,XR_XIR_U8);
            if(status!=XR_XIR_OK)return status;
        }
        return XR_XIR_OK;
    }
    if(op->type!=(op->op==XR_XIR_FIXED_ARRAY_LEN?XR_XIR_I64:XR_XIR_U8))return XR_XIR_BAD_TYPE;
    status=role_operand(function,graph,context,instruction,0,type);
    if(status==XR_XIR_OK && op->op==XR_XIR_FIXED_ARRAY_GET)
        status=role_operand(function,graph,context,instruction,1,XR_XIR_I64);
    return status;
}
