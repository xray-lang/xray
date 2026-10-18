/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture_cells.inc.c - Actual Cell dependencies and Class subjects
 *
 * KEY CONCEPT:
 *   Shared Cell proof coordinates are interpreted with the current key's
 *   separately supported initial rows and the same finite Class relation.
 */
static XrXirStatus effect_capture_cell_subject(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t destination,const XrXirCellClassRoot *root) {
    if(!root || root->place<node->head.parameter_count || root->place>=node->values || root->value>=node->values)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage;const XrXirInstruction *place=NULL;uint32_t kind=0;XrXirType type=0;
    XrXirStatus status=effect_body_request_instruction(&read->body,read->final->nodes[node->node].body,
        root->place-node->head.parameter_count,&storage,&place);
    if(status==XR_XIR_OK) status=effect_capture_kind(read,node,root->value,&kind,&type);
    if(status!=XR_XIR_OK) return status;
    if(kind!=EFFECT_CAPTURE_CLASS || place->op!=XR_XIR_OBJECT_PLACE || place->args[0]!=root->value || place->type!=type)
        return XR_XIR_BAD_STRUCTURE;
    for(uint32_t flag=0;flag<2;++flag) {
        uint32_t bit=node->basis.parameters+flag;const EffectCaptureFact *source=NULL;
        status=effect_capture_find(read,(EffectCaptureFact){node->node,root->value,node->basis.fn_words+bit/64,bit%64,0},&source);
        if(status!=XR_XIR_OK) return status;
        if(source) {
            status=effect_capture_bit(read,node,destination,node->basis.fn_words,bit,source->rank+1);
            if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_cell(EffectCaptureRead *read,const EffectCaptureNode *node,uint32_t value) {
    XrXirCellClassOriginView full={0};XrXirStatus status=xr_xir_compile_cell_class_origin_view(read->work,read->final->cells,
        read->final->nodes[node->node].body,value,&full);if(status!=XR_XIR_OK) return status;
    XrXirCellOriginView view=full.cells;uint32_t intrinsic=full.residual_intrinsic_mask;
    if(view.word_count!=((uint64_t)view.parameter_count+63)/64 || !!view.dependencies!=!!view.word_count ||
        !!view.parameters!=!!view.parameter_count || (intrinsic&~(XR_XIR_CELL_ACCESS_ROOT|XR_XIR_CELL_ACCESS_UNKNOWN)) ||
        full.word_count!=((uint64_t)full.root_count+63)/64 || !!full.dependencies!=!!full.word_count || !!full.roots!=!!full.root_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t known=node->basis.parameters+2;
    status=effect_capture_bit(read,node,value,node->basis.fn_words,known,0);if(status!=XR_XIR_OK) return status;
    for(uint32_t flag=0;flag<2;++flag) if(intrinsic&(flag?XR_XIR_CELL_ACCESS_UNKNOWN:XR_XIR_CELL_ACCESS_ROOT)) {
        status=effect_capture_bit(read,node,value,node->basis.fn_words,node->basis.parameters+flag,0);
        if(status!=XR_XIR_OK) return status;
    }
    for(uint32_t w=0;w<full.word_count;++w) {
        if(!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
        uint64_t bits=full.dependencies[w];
        while(bits) {
            if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
            uint64_t index=(uint64_t)w*64+(uint32_t)__builtin_ctzll(bits);bits&=bits-1;
            if(index>=full.root_count) return XR_XIR_BAD_STRUCTURE;
            status=effect_capture_cell_subject(read,node,value,&full.roots[index]);if(status!=XR_XIR_OK) return status;
        }
    }
    for(uint32_t bit=0;bit<view.parameter_count;++bit) {
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if(!(view.dependencies[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
        uint32_t parameter=view.parameters[bit],kind=0;
        if(parameter>=node->head.parameter_count) return XR_XIR_BAD_STRUCTURE;
        status=effect_capture_kind(read,node,parameter,&kind,NULL);if(status!=XR_XIR_OK) return status;
        if(kind!=EFFECT_CAPTURE_CELL) return XR_XIR_BAD_STRUCTURE;
        EffectCaptureTransfer edge={.from=node->node,.value=node->values+1+parameter,.to=node->node,.destination=value,
            .first=node->basis.fn_words,.word=node->basis.fn_words,.words=node->basis.cell_words};
        status=effect_capture_transfer(read,&edge);if(status!=XR_XIR_OK) return status;
        const uint64_t *input=read->final->nodes[node->node].input+(uint64_t)parameter*node->basis.row_words;
        if(!(input[node->basis.fn_words+known/64]&(UINT64_C(1)<<(known%64)))) {
            status=effect_capture_bit(read,node,value,node->basis.fn_words,known-1,0);if(status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_capture_class_intrinsic(EffectCaptureRead *read,const EffectCaptureNode *node,
    uint32_t instruction,const XrXirInstruction *op) {
    uint32_t value=node->head.parameter_count+instruction;bool opaque=op->op!=XR_XIR_CLASS_NEW;
    XrXirInstruction frontier_storage;const XrXirInstruction *frontier=op;
    if(op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_RESULT) {
        if(op->op==XR_XIR_INVOKE_RESULT) {
            if(op->immediate<0 || (uint64_t)op->immediate>=node->head.instruction_count) return XR_XIR_BAD_STRUCTURE;
            XrXirStatus status=effect_body_request_instruction(&read->body,read->final->nodes[node->node].body,
                (uint32_t)op->immediate,&frontier_storage,&frontier);if(status!=XR_XIR_OK) return status;
        }
        opaque=xr_xir_type_span(read->body.domain.types,op->type)!=0 ||
            frontier->op==XR_XIR_CALL_INDIRECT || frontier->op==XR_XIR_INVOKE_INDIRECT;
        if(!opaque && (frontier->op==XR_XIR_CALL || frontier->op==XR_XIR_INVOKE)) {
            if(frontier->immediate<0 || (uint64_t)frontier->immediate>=read->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
            EffectBodyFunctionHead target={0};XrXirStatus status=effect_body_request_head(&read->body,(uint32_t)frontier->immediate,&target);
            if(status!=XR_XIR_OK) return status;
            opaque=xr_xir_type_span(read->body.domain.types,target.result)!=0;
            for(uint32_t p=0;p<target.parameter_count;++p) {
                if(!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
                if(xr_xir_type_span(read->body.domain.types,target.parameters[p])) opaque=true;
            }
        }
        if(!opaque) return XR_XIR_OK;
    }
    XrXirStatus status=effect_capture_bit(read,node,value,node->basis.fn_words,node->basis.parameters+2,0);
    if(status==XR_XIR_OK && opaque) status=effect_capture_bit(read,node,value,node->basis.fn_words,node->basis.parameters+1,0);
    if(status==XR_XIR_OK) status=effect_capture_emit(read,(EffectCaptureFact){node->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_RAW,0});
    return status;
}

/* These are untrusted constructor proposals with deliberately non-authoritative
 * ranks. Only actual Input clauses may lower them into a finite proof. The
 * receiving matcher never reads this retired construction inventory. */
static XrXirStatus effect_capture_class_proposals(EffectCaptureRead *read,
    const EffectInvocationWorkspace *discovery) {
    if(!read->active) return XR_XIR_OK;
    if(!discovery || read->final->nodes!=discovery->final.nodes ||
        read->final->count!=discovery->final.count || !discovery->states) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus frame=effect_role_range(read->work,discovery->states,discovery->final.capacity,sizeof(*discovery->states));
    if(frame!=XR_XIR_OK) return frame;
    for(uint32_t n=0;n<read->final->count;++n) {
        const EffectInvocationClassDiscovery *source=&discovery->states[n].classes;
        if(!xir_compile_work(read->work,2)) return XR_XIR_BUDGET;
        if(!source->count && !source->return_mask) continue;
        EffectCaptureNode node={0};XrXirStatus status=effect_capture_node(read,n,&node);if(status!=XR_XIR_OK) return status;
        status=effect_role_range(read->work,source->facts,source->count,sizeof(*source->facts));
        if(status!=XR_XIR_OK) return status;
        for(uint32_t c=0;c<=source->count;++c) {
            uint32_t value=c==source->count?node.values:source->facts[c].value;
            uint32_t mask=c==source->count?source->return_mask:source->facts[c].mask;
            if(!mask) continue;
            uint32_t kind=0;status=effect_capture_kind(read,&node,value,&kind,NULL);if(status!=XR_XIR_OK) return status;
            if(kind!=EFFECT_CAPTURE_CLASS || (mask&~EFFECT_CLASS_VALID)) return XR_XIR_BAD_STRUCTURE;
            for(uint32_t flag=0;flag<3;++flag) {
                uint32_t present=flag==0?EFFECT_CLASS_ROOT:flag==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
                if(!(mask&present)) continue;
                status=effect_capture_bit(read,&node,value,node.basis.fn_words,node.basis.parameters+flag,read->maximum-1);
                if(status!=XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}
