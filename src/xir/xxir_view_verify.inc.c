/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_verify.inc.c - Exact operand shapes for borrowed storage operations
 *
 * KEY CONCEPT:
 *   Type checking precedes independent root, path and lifetime verification.
 */
static bool view_operand_kind(const XrXirTypeNode *node) {
    return node && (node->kind==XR_XIR_TYPE_SLICE || node->kind==XR_XIR_TYPE_MUT_SLICE);
}
static XrXirStatus view_cleanup_uses(const XrXirFunction *function,VerifyContext *context,uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    if (op->immediate) return XR_XIR_BAD_STRUCTURE;
    uint8_t *flags=NULL;XrXirStatus status=xir_compile_view_flags_new(&context->remaining,context->module->types,&flags);
    for (uint32_t p=0;status==XR_XIR_OK && p<op->args[1];++p) {
        if (!xir_compile_work(&context->remaining,2)) {status=XR_XIR_BUDGET;break;}
        uint32_t value=function->operands[op->args[0]+p];
        if (value>=(uint64_t)function->parameter_count+function->instruction_count ||
            (p && value<=function->operands[op->args[0]+p-1])) {status=XR_XIR_BAD_STRUCTURE;break;}
        XrXirOp producer=value<function->parameter_count?XR_XIR_INVALID:function->instructions[value-function->parameter_count].op;
        XrXirType type=xr_xir_operand_type(function,value);
        if (producer==XR_XIR_INVOKE || producer==XR_XIR_INVOKE_INDIRECT || producer==XR_XIR_INVOKE_DEFAULT ||
            !xr_xir_type_node(context->module->types,type) || !flags ||
            !(flags[(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE]&3u)) status=XR_XIR_BAD_TYPE;
    }
    xr_compile_resources_free(flags);return status;
}
static XrXirStatus view_uses(const Graph *graph,const XrXirFunction *function,
    VerifyContext *context,uint32_t instruction) {
    const XrXirInstruction *op=&function->instructions[instruction];
    if (op->op==XR_XIR_VIEW_CLEANUP) return view_cleanup_uses(function,context,instruction);
    const XrXirTypes *types=context->module->types;
    bool group=xr_xir_op_uses_operand_table(op->op);
    if (group && op->args[1]!=3) return XR_XIR_BAD_STRUCTURE;
    if ((op->op==XR_XIR_VIEW_NEW || op->op==XR_XIR_VIEW_REBORROW) && op->immediate) {
        uint32_t end=function->operands[op->args[0]+2];
        if (end<function->parameter_count || end-function->parameter_count>=function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *zero=&function->instructions[end-function->parameter_count];
        if (zero->op!=XR_XIR_CONST_INT || zero->type!=XR_XIR_I64 || zero->immediate) return XR_XIR_BAD_STRUCTURE;
    }
    uint32_t first=group ? function->operands[op->args[0]] : op->args[0];
    XrXirType source=xr_xir_operand_type(function,first);
    const XrXirTypeNode *input=xr_xir_type_node(types,source);
    const XrXirTypeNode *output=xr_xir_type_node(types,op->type);
    XrXirType element=XR_XIR_UNIT;
    if (op->op==XR_XIR_VIEW_END) {
        uint32_t flags=0;
        XrXirStatus status=xr_xir_compile_view_type_flags(&context->remaining,types,source,&flags);
        if (status!=XR_XIR_OK) return status;
        if (!flags) return XR_XIR_BAD_TYPE;
    } else if (op->op==XR_XIR_VIEW_NEW) {
        if (!input || input->kind!=XR_XIR_TYPE_CELL || !view_operand_kind(output)) return XR_XIR_BAD_TYPE;
        const XrXirTypeNode *array=xr_xir_type_node(types,input->element);
        bool string=input->element==XR_XIR_STRING;
        if (string ? output->kind!=XR_XIR_TYPE_SLICE || output->element!=XR_XIR_U8 :
            !array || array->kind!=XR_XIR_TYPE_ARRAY || array->element!=output->element) return XR_XIR_BAD_TYPE;
    } else {
        if (!view_operand_kind(input)) return XR_XIR_BAD_TYPE;
        element=input->element;
        if (op->op==XR_XIR_VIEW_REBORROW && (!view_operand_kind(output) || output->element!=element ||
            (output->kind==XR_XIR_TYPE_MUT_SLICE && input->kind!=XR_XIR_TYPE_MUT_SLICE))) return XR_XIR_BAD_TYPE;
        if (op->op==XR_XIR_VIEW_GET && op->type!=element) return XR_XIR_BAD_TYPE;
        if (op->op==XR_XIR_VIEW_SET && input->kind!=XR_XIR_TYPE_MUT_SLICE) return XR_XIR_BAD_TYPE;
        if (op->op==XR_XIR_VIEW_COPY && (!output || output->kind!=XR_XIR_TYPE_ARRAY || output->element!=element))
            return XR_XIR_BAD_TYPE;
    }
    XrXirStatus status=role_operand(function,graph,context,instruction,0,source);
    uint32_t count=group ? 3u : op->op==XR_XIR_VIEW_GET ? 2u : 1u;
    for (uint32_t i=1;i<count && status==XR_XIR_OK;++i)
        status=role_operand(function,graph,context,instruction,i,
            op->op==XR_XIR_VIEW_SET && i==2 ? element : XR_XIR_I64);
    return status;
}
