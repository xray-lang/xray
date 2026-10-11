/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_cell_read.inc.c - One body reader for fresh Cell replay
 *
 * This stack request borrows one admitted generation for a finite operation.
 * Provider correspondence adds no permission and retains no producer.
 */
typedef struct EffectCellBodyAdapter {
    const XrXirCompileContext *work;
    EffectBodyRequest request;
} EffectCellBodyAdapter;

static bool effect_cell_body_adapter_valid(const XrXirCompileContext *work,
    const EffectCellBodyAdapter *adapter) {
    return xir_compile_context_valid(work) && adapter && xir_compile_context_valid(adapter->work) &&
        adapter->work->resources==work->resources && adapter->request.owner &&
        adapter->request.read.owner==adapter->work &&
        adapter->request.read.consume==effect_body_compile_consume &&
        adapter->request.head && adapter->request.instruction && adapter->request.type && adapter->request.shape &&
        adapter->request.domain.types;
}
static XrXirStatus effect_cell_body_head(const XrXirCompileContext *work,const void *owner,
    uint32_t f,XrXirCellProofBodyHead *output) {
    const EffectCellBodyAdapter *adapter=owner;
    if (!effect_cell_body_adapter_valid(work,adapter) || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,11)) return XR_XIR_BUDGET;
    EffectBodyFunctionHead head={0};
    XrXirStatus status=effect_body_request_head(&adapter->request,f,&head);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,6+sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirCellProofBodyHead){head.parameters,head.parameter_count,head.result,
        head.instruction_count,head.operands,head.operand_count};return XR_XIR_OK;
}
static XrXirStatus effect_cell_body_instruction(const XrXirCompileContext *work,const void *owner,
    uint32_t f,uint32_t i,XrXirInstruction *output) {
    const EffectCellBodyAdapter *adapter=owner;
    if (!effect_cell_body_adapter_valid(work,adapter) || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,11)) return XR_XIR_BUDGET;
    XrXirInstruction storage={0};const XrXirInstruction *operation=NULL;
    XrXirStatus status=effect_body_request_instruction(&adapter->request,f,i,&storage,&operation);
    if (status!=XR_XIR_OK) return status;
    if (!operation) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,1+sizeof(*output))) return XR_XIR_BUDGET;
    *output=*operation;return XR_XIR_OK;
}
static XrXirStatus effect_cell_body_type(const XrXirCompileContext *work,const void *owner,
    uint32_t f,uint32_t value,XrXirType *output) {
    const EffectCellBodyAdapter *adapter=owner;
    if (!effect_cell_body_adapter_valid(work,adapter) || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,11)) return XR_XIR_BUDGET;
    XrXirType type=XR_XIR_UNIT;
    XrXirStatus status=effect_body_request_type(&adapter->request,f,value,&type);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=type;return XR_XIR_OK;
}
static XrXirStatus effect_cell_body_shape(const XrXirCompileContext *work,const void *owner,
    uint32_t f,uint32_t i,XrXirCellProofInstructionShape *output) {
    const EffectCellBodyAdapter *adapter=owner;
    if (!effect_cell_body_adapter_valid(work,adapter) || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,11+sizeof(EffectBodyInstructionShape))) return XR_XIR_BUDGET;
    EffectBodyInstructionShape shape={0};
    XrXirStatus status=effect_body_request_shape(&adapter->request,f,i,&shape);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirCellProofInstructionShape){shape.op,shape.type};return XR_XIR_OK;
}
static XrXirStatus effect_cell_body_bind(const XrXirCompileContext *work,
    const EffectBodyRequest *request,EffectCellBodyAdapter *adapter,XrXirCellProofBodyRead *output) {
    if (!xir_compile_context_valid(work) || !request || !adapter || !output || !request->owner ||
        request->read.owner!=work || request->read.consume!=effect_body_compile_consume ||
        !request->domain.types || !request->head || !request->instruction || !request->type || !request->shape)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,10+sizeof(*adapter)+sizeof(*output))) return XR_XIR_BUDGET;
    *adapter=(EffectCellBodyAdapter){work,*request};
    *output=(XrXirCellProofBodyRead){request->domain,work->resources,adapter,
        effect_cell_body_head,effect_cell_body_instruction,effect_cell_body_type,effect_cell_body_shape};return XR_XIR_OK;
}
/* Getter identity selects only the genuine storage layout. It supplies no
 * permission fact; full admission and independent source matching remain
 * prerequisites to the fresh provider operation. */
static inline XrXirStatus effect_cell_body_request(const XrXirCompileContext *work,
    const EffectBodyRequest *request,EffectCellBodyAdapter *adapter,XrXirCellProofBodyRead *output) {
    if (!xir_compile_context_valid(work) || !request || !request->owner) return XR_XIR_BAD_STRUCTURE;
    XirMetadataDomain domain={0};
    if (request->head==effect_body_raw_head && request->instruction==effect_body_raw_instruction &&
        request->type==effect_body_raw_type && request->shape==effect_body_raw_shape) {
        const XrXirModule *module=request->owner;
        if (module->function_count && !module->functions) return XR_XIR_BAD_STRUCTURE;
        domain=xir_metadata_domain_from_module(module);
        if (!domain.types) domain.types=&effect_empty_types;
    } else if (request->head==effect_body_owned_head && request->instruction==effect_body_owned_instruction &&
        request->type==effect_body_owned_type && request->shape==effect_body_owned_shape) {
        const EffectBodyMaterial *body=request->owner;
        if (!effect_body_material_shape(body) || body->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
        domain=body->domain;
    } else return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,10)) return XR_XIR_BUDGET;
    if (request->domain.types!=domain.types || request->domain.declarations!=domain.declarations ||
        request->domain.generics!=domain.generics || request->domain.defaults!=domain.defaults ||
        request->domain.function_count!=domain.function_count) return XR_XIR_BAD_STRUCTURE;
    return effect_cell_body_bind(work,request,adapter,output);
}
/* The raw constructor preserves the verified-caller precondition. It does
 * not replace full shape, type, SSA, initialization or external matching. */
static inline XrXirStatus effect_cell_body_raw(const XrXirCompileContext *work,
    const XrXirModule *module,EffectCellBodyAdapter *adapter,XrXirCellProofBodyRead *output) {
    if (!xir_compile_context_valid(work) || !module ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_raw_request(work,module);
    return effect_cell_body_request(work,&request,adapter,output);
}
static inline XrXirStatus effect_cell_body_owned(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,EffectCellBodyAdapter *adapter,XrXirCellProofBodyRead *output) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) ||
        body->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(work,body);
    return effect_cell_body_request(work,&request,adapter,output);
}
