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
    uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    const EffectCellBodyAdapter *adapter=owner;
    if (!effect_cell_body_adapter_valid(work,adapter) || !storage || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,11)) return XR_XIR_BUDGET;
    const XrXirInstruction *operation=NULL;
    XrXirStatus status=effect_body_request_instruction(&adapter->request,f,i,storage,&operation);
    if (status!=XR_XIR_OK) return status;
    if (!operation) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,1+sizeof(*output))) return XR_XIR_BUDGET;
    *output=operation;return XR_XIR_OK;
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
/* A prefix is an unfinished inventory, never a complete body proof. Count
 * zero is the independent end request: every row, run and stream word must
 * then agree with the complete owned head, including the last EOF boundary. */
static XrXirStatus effect_cell_body_span(const XrXirCompileContext *work,const void *owner,
    uint32_t f,uint32_t first,uint32_t count,XrXirCellProofSpanCursor *cursor) {
    const EffectCellBodyAdapter *adapter=owner;
    if (!effect_cell_body_adapter_valid(work,adapter) || !cursor ||
        adapter->request.head!=effect_body_owned_head || adapter->request.instruction!=effect_body_owned_instruction ||
        adapter->request.type!=effect_body_owned_type || adapter->request.shape!=effect_body_owned_shape)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body=adapter->request.owner;
    if (!effect_body_material_shape(body) || body->resources!=work->resources || f>=effect_body_count(body) ||
        cursor->function!=f) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *function=&body->functions[f];

    if (function->literals) {
        if (!effect_body_function_storage_shape(function) || count || first!=function->instruction_count ||
            cursor->next>first || cursor->run!=cursor->next) return XR_XIR_BAD_STRUCTURE;
        XrXirCellProofSpanCursor current=*cursor;
        for (uint32_t i=current.next;i<first;++i) {
            if (!xir_compile_work(work,3+sizeof(uint32_t))) return XR_XIR_BUDGET;
            XrXirOp opcode=function->literals[i].op;
            if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        }
        if (!xir_compile_work(work,sizeof(*cursor))) return XR_XIR_BUDGET;
        current.next=first;current.run=first;*cursor=current;return XR_XIR_OK;
    }
    if (first>function->instruction_count || count>function->instruction_count-first ||
        (!count && first!=function->instruction_count) || cursor->next>first ||
        cursor->run>function->run_count || function->run_count>function->instruction_count ||
        function->run_count>function->run_capacity) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,14+sizeof(*cursor))) return XR_XIR_BUDGET;
    XrXirCellProofSpanCursor current=*cursor;
    while (current.run<function->run_count) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        const EffectBodyRun *ref;
        XrXirStatus status=effect_body_run_ref(&adapter->request.read,function,current.run,current.next,&ref);
        if (status!=XR_XIR_OK) return status;
        if (current.run+1<function->run_count) {
            const EffectBodyRun *neighbor;
            status=effect_body_run_ref(&adapter->request.read,function,current.run+1,ref->first+ref->count,&neighbor);
            if (status!=XR_XIR_OK) return status;
        }
        if (count && current.next==first) {
            bool varying=!!(ref->mask&UINT32_C(3));
            if (ref->count==2) {
                uint32_t opcode=0,type=0;
                status=effect_body_run_word(&adapter->request.read,ref,ref->first+1,0,&opcode);
                if (status==XR_XIR_OK) status=effect_body_run_word(&adapter->request.read,ref,ref->first+1,1,&type);
                if (status!=XR_XIR_OK) return status;
                if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                varying=opcode!=(uint32_t)ref->base.op || type!=(uint32_t)ref->base.type;
            }
            if (ref->count!=count || varying) return XR_XIR_BAD_TYPE;
            if (ref->base.op<=XR_XIR_INVALID || ref->base.op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(work,2+sizeof(current.shape))) return XR_XIR_BUDGET;
            current.shape=(XrXirCellProofInstructionShape){(XrXirOp)ref->base.op,(XrXirType)ref->base.type};
        } else if (current.next>=first || ref->count>first-current.next) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,2+sizeof(current.next)+sizeof(current.run))) return XR_XIR_BUDGET;
        current.next+=ref->count;++current.run;
        if (count && current.next==first+count) {
            if (!xir_compile_work(work,sizeof(*cursor))) return XR_XIR_BUDGET;
            *cursor=current;return XR_XIR_OK;
        }
    }
    if (count || current.next!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*cursor))) return XR_XIR_BUDGET;
    *cursor=current;return XR_XIR_OK;
}

/* Only this complete owned adapter can expose a canonical range. Callback
 * identity selects layout and rejects mixed requests; it grants no authority.
 * Every run decodes all ten first-row words through the sole physical reader. */
static XrXirStatus effect_cell_body_range(const XrXirCompileContext *work,
    const XrXirCellProofBodyRead *read,XrXirCellProofRunCursor *cursor,XrXirCellProofRun *output) {
    EffectBodyRun dense_range_storage;
    if (!xir_compile_context_valid(work) || !read || !cursor || !output || read->resources!=work->resources ||
        read->head!=effect_cell_body_head || read->instruction!=effect_cell_body_instruction ||
        read->type!=effect_cell_body_type || read->shape!=effect_cell_body_shape ||
        read->range!=effect_cell_body_range) return XR_XIR_BAD_STRUCTURE;
    const EffectCellBodyAdapter *adapter=read->owner;
    if (!effect_cell_body_adapter_valid(work,adapter) ||
        adapter->request.head!=effect_body_owned_head || adapter->request.instruction!=effect_body_owned_instruction ||
        adapter->request.type!=effect_body_owned_type || adapter->request.shape!=effect_body_owned_shape)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body=adapter->request.owner;
    if (!effect_body_material_shape(body) || body->resources!=work->resources ||
        read->domain.types!=body->domain.types || read->domain.declarations!=body->domain.declarations ||
        read->domain.generics!=body->domain.generics || read->domain.defaults!=body->domain.defaults ||
        read->domain.function_count!=effect_body_count(body) || cursor->function>=effect_body_count(body))
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *function=&body->functions[cursor->function];
    if (cursor->next>function->instruction_count || cursor->run>function->run_count ||
        (!cursor->run && cursor->next)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,18+sizeof(EffectBodyRecordCursor))) return XR_XIR_BUDGET;
    EffectBodyRecordCursor position={cursor->next,cursor->run,UINT32_MAX};
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_range_take(&adapter->request.read,function,&position,&ref,&dense_range_storage);
    if (status!=XR_XIR_OK) return status;
    status=effect_body_instruction_ref_prepare(&adapter->request.read,ref,ref->first);
    if (status!=XR_XIR_OK) return status;
    XrXirInstruction operation=ref->base;
    uint32_t varying=ref->mask;
    if (ref->count==2) {
        if (!xir_compile_work(work,sizeof(varying))) return XR_XIR_BUDGET;
        varying=0;
        for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
            if (effect_body_source_word(&ref->base,w)!=ref->delta[w]) varying|=UINT32_C(1)<<w;
        }
    }
    if (!xir_compile_work(work,sizeof(*output)+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    *output=(XrXirCellProofRun){ref->first,ref->count,varying,operation};
    cursor->next=position.first;cursor->run=position.run;return XR_XIR_OK;
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
        effect_cell_body_head,effect_cell_body_instruction,effect_cell_body_type,effect_cell_body_shape,
        request->head==effect_body_owned_head?effect_cell_body_span:NULL,
        request->head==effect_body_owned_head?effect_cell_body_range:NULL};return XR_XIR_OK;
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
