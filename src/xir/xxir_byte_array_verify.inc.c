/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_byte_array_verify.inc.c - Exact owned byte candidate operands
 *
 * KEY CONCEPT:
 *   Candidate operations grant no place or borrowed-view authority.
 */
static XrXirStatus byte_array_uses(const Graph *graph,const XrXirFunction *function,
    VerifyContext *context,uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    const XrXirTypes *types=context->module->types;
    if (!xr_xir_type_is_array(types,op->type) || xr_xir_array_element(types,op->type)!=XR_XIR_U8)
        return XR_XIR_BAD_TYPE;
    bool repeat=op->op==XR_XIR_BYTE_ARRAY_REPEAT;
    if (op->immediate || op->targets[0] || op->targets[1] ||
        op->type_arguments[0] || op->type_arguments[1] || (repeat && op->args[1]!=3))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<(repeat?3u:2u);++p) {
        XrXirType expected=repeat && p?XR_XIR_I64:op->type;
        XrXirStatus status=role_operand(function,graph,context,instruction,p,expected);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
