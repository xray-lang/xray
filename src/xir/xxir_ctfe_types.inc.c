/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_types.inc.c - Iterative compiler value-family admission
 *
 * KEY CONCEPT:
 *   Local IDs have meaning only in their verified immutable pool. An explicit
 *   ledger-owned DFS rejects cyclic or authority-bearing Tuple/Nullable fields.
 */
typedef struct CtfeTypeFrame { uint64_t work;uint32_t index,next; } CtfeTypeFrame;
static const XrXirTypeNode *ctfe_composite_node(const XrXirTypes *types,XrXirType type) {
    const XrXirTypeNode *node=xr_xir_type_node(types,type);
    return node && (node->kind==XR_XIR_TYPE_TUPLE || node->kind==XR_XIR_TYPE_NULLABLE)?node:NULL;
}
static uint32_t ctfe_composite_fields(const XrXirTypeNode *node) {
    return node->kind==XR_XIR_TYPE_NULLABLE?1:node->parameter_count;
}
static XrXirCtfeStatus ctfe_type_info(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type,uint64_t *output) {
    if (!xir_compile_context_valid(context)) return XR_XIR_CTFE_BAD_ARGUMENT;
    if (ctfe_result_type(type)) { *output=1;return XR_XIR_CTFE_OK; }
    const XrXirTypeNode *root=ctfe_composite_node(types,type);
    if (!root || !types || !types->count) return XR_XIR_CTFE_NOT_SUPPORTED;
    uint64_t bytes=(uint64_t)types->count*(sizeof(CtfeTypeFrame)+sizeof(uint64_t)+1);
    if (bytes>SIZE_MAX) return XR_XIR_CTFE_BUDGET;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(context->resources,1,(size_t)bytes,&memory)));
    if (status!=XR_XIR_CTFE_OK) return status;
    CtfeTypeFrame *stack=memory;uint64_t *costs=(uint64_t *)(stack+types->count);
    uint8_t *colors=(uint8_t *)(costs+types->count);uint32_t depth=1;
    if (!xir_compile_work(context,sizeof(*stack)+1)) { status=XR_XIR_CTFE_BUDGET;goto done; }
    uint32_t index=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE;
    stack[0]=(CtfeTypeFrame){(uint64_t)ctfe_composite_fields(root)+1,index,0};colors[index]=1;
    while (depth) {
        if (!xir_compile_work(context,4)) { status=XR_XIR_CTFE_BUDGET;break; }
        CtfeTypeFrame *frame=&stack[depth-1];
        const XrXirTypeNode *node=&types->nodes[frame->index];
        bool nullable=node->kind==XR_XIR_TYPE_NULLABLE;
        if ((!nullable && (node->kind!=XR_XIR_TYPE_TUPLE || !node->parameter_count || !node->parameters)) || node->parameter_span) {
            status=XR_XIR_CTFE_NOT_SUPPORTED;break;
        }
        if (frame->next==ctfe_composite_fields(node)) {
            colors[frame->index]=2;costs[frame->index]=frame->work;--depth;
            if (!depth) { *output=frame->work;break; }
            CtfeTypeFrame *parent=&stack[depth-1];
            if (frame->work>UINT64_MAX-parent->work) { status=XR_XIR_CTFE_BUDGET;break; }
            parent->work+=frame->work;continue;
        }
        if (!xir_compile_work(context,sizeof(XrXirCallableParameter)+2)) { status=XR_XIR_CTFE_BUDGET;break; }
        const XrXirCallableParameter field=nullable?(XrXirCallableParameter){node->element,0}:node->parameters[frame->next];
        ++frame->next;
        if (field.mode || (nullable && field.type==XR_XIR_UNIT)) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        if (ctfe_result_type(field.type)) {
            if (frame->work==UINT64_MAX) { status=XR_XIR_CTFE_BUDGET;break; }
            ++frame->work;continue;
        }
        const XrXirTypeNode *child=ctfe_composite_node(types,field.type);
        if (!child) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        index=(uint32_t)field.type-XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (colors[index]==1) { status=XR_XIR_CTFE_NOT_SUPPORTED;break; }
        if (colors[index]==2) {
            if (costs[index]>UINT64_MAX-frame->work) { status=XR_XIR_CTFE_BUDGET;break; }
            frame->work+=costs[index];continue;
        }
        if (depth>=types->count) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
        if (!xir_compile_work(context,sizeof(*stack)+1)) { status=XR_XIR_CTFE_BUDGET;break; }
        stack[depth++]=(CtfeTypeFrame){(uint64_t)ctfe_composite_fields(child)+1,index,0};colors[index]=1;
    }
done:
    xr_compile_resources_free(memory);return status;
}
static XrXirCtfeStatus ctfe_equal_admit(const XrXirArtifact *artifact,XrXirType type) {
    for (;;) {
        if (!xir_compile_work(&artifact->context,2)) return XR_XIR_CTFE_BUDGET;
        if (ctfe_parameter_type(type)) return XR_XIR_CTFE_OK;
        const XrXirTypeNode *node=xr_xir_type_node(artifact->module.types,type);
        if (!node || node->kind!=XR_XIR_TYPE_NULLABLE) return XR_XIR_CTFE_NOT_SUPPORTED;
        type=node->element;
    }
}
XR_FUNC XrXirCtfeStatus xr_xir_compile_ctfe_type_admit(const XrXirCompileContext *context,
    const XrXirTypes *types,XrXirType type) {
    uint64_t work=0;return ctfe_type_info(context,types,type,&work);
}
static XrXirCtfeStatus ctfe_body_type_admit(const XrXirArtifact *artifact,XrXirType type,bool parameter) {
    if (parameter && type==XR_XIR_UNIT) return XR_XIR_CTFE_NOT_SUPPORTED;
    return xr_xir_compile_ctfe_type_admit(&artifact->context,artifact->module.types,type);
}
static XrXirCtfeStatus ctfe_tuple_instruction_admit(const XrXirArtifact *artifact,
    const XrXirFunction *body,const XrXirInstruction *op) {
    XrXirType type=op->type;
    if (op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE)
        type=xr_xir_operand_type(body,op->args[0]);
    if (xr_xir_type_is_cell(artifact->module.types,type))
        type=xr_xir_cell_element(artifact->module.types,type);
    XrXirCtfeStatus status=ctfe_body_type_admit(artifact,type,false);
    if (status==XR_XIR_CTFE_OK && (op->op==XR_XIR_TUPLE_FIELD || op->op==XR_XIR_NULLABLE_IS_SOME || op->op==XR_XIR_NULLABLE_UNWRAP))
        status=ctfe_body_type_admit(artifact,xr_xir_operand_type(body,op->args[0]),false);
    return status;
}
static XrXirCtfeStatus ctfe_tuple_scratch(const XrXirArtifact *artifact,uint32_t function,uint64_t *bytes) {
    *bytes=0;
    if (!artifact->layouts[function].outgoing_count) return XR_XIR_CTFE_OK;
    const XrXirFunction *body=&artifact->module.functions[function];uint32_t count=0;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        if (!xir_compile_work(&artifact->context,1)) return XR_XIR_CTFE_BUDGET;
        if (body->instructions[i].op!=XR_XIR_TUPLE_NEW) continue;
        const XrXirTypeNode *tuple=xr_xir_tuple_signature(artifact->module.types,body->instructions[i].type);
        if (!tuple || tuple->parameter_count>artifact->layouts[function].outgoing_count) return XR_XIR_CTFE_BAD_ARTIFACT;
        if (tuple->parameter_count>count) count=tuple->parameter_count;
    }
    *bytes=(uint64_t)count*sizeof(XrXirValue);return XR_XIR_CTFE_OK;
}
