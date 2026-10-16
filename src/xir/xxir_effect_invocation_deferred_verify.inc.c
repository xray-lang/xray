/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_deferred_verify.inc.c - Exact delayed frontier closure
 *
 * KEY CONCEPT:
 *   A delayed invocation owns the real formal and complete physical actuals.
 *   Replaying the common value-origin flow verifies this correspondence before
 *   seal; it neither solves another invocation graph nor publishes permission.
 */
static XrXirStatus effect_invocation_deferred_match(EffectInvocationFlow *flow,
    const EffectInvocationDeferred *record) {
    const EffectInvocationRead *owner=flow->read;
    if (record->node!=flow->node || record->instruction>=flow->function->instruction_count ||
        record->parameter>=flow->basis.parameters || !!record->arguments!=!!record->count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction body_op_1;const XrXirInstruction *op=NULL;
    XrXirStatus body_read_1=effect_invocation_flow_instruction(flow,record->instruction,&body_op_1,&op);
    if (body_read_1!=XR_XIR_OK) return body_read_1;
    if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
        op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirType actual_type=0;
    XrXirStatus type_status=effect_invocation_flow_type(flow,(uint32_t)op->immediate,&actual_type);
    if (type_status!=XR_XIR_OK) return type_status;
    const XrXirTypeNode *signature=xr_xir_callable_signature(flow->body.domain.types,actual_type);
    bool ref=false;XrXirStatus status=effect_invocation_has_ref(flow,signature,&ref);
    if (status!=XR_XIR_OK) return status;
    if (!ref || record->count!=signature->parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t bit=owner->final->site_count+record->parameter;
    const uint64_t *descriptor=effect_invocation_row(flow,(uint32_t)op->immediate);
    if (!descriptor) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
    if (!(descriptor[bit/64]&(UINT64_C(1)<<(bit%64)))) return XR_XIR_BAD_STRUCTURE;
    uint64_t *actual=NULL;flow->instruction=record->instruction;
    status=effect_invocation_deferred_arguments(flow,record->instruction,signature,&actual);
    uint64_t words=(uint64_t)record->count*flow->basis.row_words;
    if (status==XR_XIR_OK && words>SIZE_MAX/sizeof(*actual)) status=XR_XIR_BUDGET;
    for (size_t w=0;status==XR_XIR_OK && w<(size_t)words;++w) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        if (record->arguments[w]!=actual[w]) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(actual);return status;
}

/* Every real symbolic REF callee has exactly one local record per root formal.
 * Known producer alternatives remain separate executing edges. Direct generic,
 * default and requirement conditions retain their existing opaque obligations. */
static XrXirStatus effect_invocation_deferred_frontiers(EffectInvocationFlow *flow,
    uint32_t *matched) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationNode *node=&owner->final->nodes[flow->node];
    const uint64_t *local_contexts=effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true);
    EffectBodyRecordCursor cursor;
    XrXirStatus sequence=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (sequence!=XR_XIR_OK) return sequence;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_2;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_2=effect_body_request_sequence_instruction(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&body_op_2,&op);
        if (body_read_2!=XR_XIR_OK) return body_read_2;
        if (!(local_contexts[i/64]&(UINT64_C(1)<<(i%64))) ||
            (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT)) continue;
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirType actual_type=0;
        XrXirStatus type_status=effect_invocation_flow_type(flow,(uint32_t)op->immediate,&actual_type);
        if (type_status!=XR_XIR_OK) return type_status;
        const XrXirTypeNode *signature=xr_xir_callable_signature(flow->body.domain.types,actual_type);
        bool ref=false;XrXirStatus status=effect_invocation_has_ref(flow,signature,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!ref) continue;
        const uint64_t *descriptor=effect_invocation_row(flow,(uint32_t)op->immediate);
        if (!descriptor) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;p<flow->basis.parameters;++p) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint32_t bit=owner->final->site_count+p;
            if (!(descriptor[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
            uint32_t selected=UINT32_MAX;
            for (uint32_t d=0;d<owner->final->deferred_count;++d) {
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                const EffectInvocationDeferred *record=&owner->final->deferred_calls[d];
                if (record->node!=flow->node || record->instruction!=i || record->parameter!=p) continue;
                if (selected!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
                selected=d;
            }
            if (selected==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            status=effect_invocation_deferred_match(flow,&owner->final->deferred_calls[selected]);
            if (status!=XR_XIR_OK) return status;
            if (*matched==UINT32_MAX) return XR_XIR_BUDGET;
            ++*matched;
        }
    }
    return effect_body_request_sequence_finish(&flow->body,flow->read->final->nodes[flow->node].body,&cursor);
}

static XrXirStatus effect_invocation_deferred_needed(const EffectInvocationRead *owner,
    uint32_t n,bool *output) {
    if (!owner || n>=owner->final->count || !output) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final->nodes[n];
    const uint64_t *contexts=effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true);
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_invocation_dimensions(owner,node->body,&head);
    if (status!=XR_XIR_OK) return status;
    uint64_t values=(uint64_t)head.parameter_count+head.instruction_count;
    bool needed=false;EffectInvocationFlow flow={.read=owner,.node=n,.body=effect_invocation_body(owner)};
    for (uint32_t i=0;i<head.instruction_count && node->local_deferred;++i) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        if (!(contexts[i/64]&(UINT64_C(1)<<(i%64)))) continue;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_invocation_operation(owner,node->body,i,&storage,&op);
        if (status!=XR_XIR_OK) return status;
        if (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) continue;
        if (op->immediate<0 || (uint64_t)op->immediate>=values) return XR_XIR_BAD_STRUCTURE;
        XrXirType type;status=effect_body_request_type(&flow.body,node->body,(uint32_t)op->immediate,&type);
        if (status!=XR_XIR_OK) return status;
        const XrXirTypeNode *signature=xr_xir_callable_signature(flow.body.domain.types,type);
        bool ref=false;status=effect_invocation_has_ref(&flow,signature,&ref);
        if (status!=XR_XIR_OK) return status;
        needed|=ref;
    }
    *output=needed;return XR_XIR_OK;
}

/* The local SSA replay uses the same immutable input and common Cell proof.
 * All records, including missing and duplicate frontiers, are consumed before
 * any certificate ownership transfer. A failure leaves every producer owned. */
static XrXirStatus effect_invocation_deferred_verify(const EffectInvocationRead *owner) {
    if (!owner || !effect_body_material_shape(owner->final->material) || !owner->effects || !owner->work ||
        owner->final->deferred_count>owner->final->deferred_capacity ||
        (owner->final->deferred_count && !owner->final->deferred_calls)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t d=0;d<owner->final->deferred_count;++d) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        const EffectInvocationDeferred *record=&owner->final->deferred_calls[d];
        if (record->node>=owner->final->count || !owner->final->nodes[record->node].local_deferred)
            return XR_XIR_BAD_STRUCTURE;
    }
    uint32_t matched=0;
    for (uint32_t n=0;n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        bool needed=false;XrXirStatus status=effect_invocation_deferred_needed(owner,n,&needed);
        if (status!=XR_XIR_OK) return status;
        if (!needed) continue;
        EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX};
        status=effect_invocation_origins(&flow);
        if (status==XR_XIR_OK) status=effect_invocation_deferred_frontiers(&flow,&matched);
        xr_compile_resources_free(flow.memory);
        if (status!=XR_XIR_OK) return status;
    }
    return matched==owner->final->deferred_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* Both lifetime adapters compare the actual final vector to the same
 * complete canonical enumeration. No expected formula or term copy is made. */
static XrXirStatus effect_invocation_result_match_source(const EffectInvocationTermSource *source,
    const EffectFinalFunctions *result) {
    if (!source || !source->equations || !xir_compile_context_valid(source->work) ||
        !effect_body_material_shape(source->equations->material) || !source->equations->roots ||
        !source->equations->nodes || !result ||
        result->resources!=source->work->resources || !result->count ||
        result->count!=effect_body_count(source->equations->material) || result->capacity!=result->count ||
        !result->functions) return XR_XIR_BAD_STRUCTURE;
    uint64_t total=0;XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<result->count && status==XR_XIR_OK;++f) {
        const EffectFinalFunction *record=&result->functions[f];
        const XrXirRootFormula *formula=&record->formula;
        if (!xir_compile_work(source->work,6)) { status=XR_XIR_BUDGET;break; }
        if (source->equations->roots[f]>=source->equations->count || formula->term_count!=record->capacity ||
            !!record->storage!=!!record->capacity || formula->terms!=record->storage ||
            formula->constant_mask!=source->equations->nodes[source->equations->roots[f]].intrinsic_mask) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (!xir_compile_work(source->work,sizeof(EffectInvocationTermTarget))) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationTermTarget target={record->capacity,0,0,NULL};uint32_t count=0;
        status=effect_invocation_result_terms(source,f,NULL,record->storage,&target,&count);
        if (status==XR_XIR_OK && count!=record->capacity) status=XR_XIR_BAD_STRUCTURE;
        total+=count;
    }
    if (status==XR_XIR_OK && total!=result->term_count) status=XR_XIR_BAD_STRUCTURE;
    return status;
}

static XrXirStatus effect_invocation_result_match(EffectInvocationWorkspace *owner,
    const EffectFinalFunctions *result) {
    if (!owner || !xir_compile_context_valid(owner->read.work) || !owner->effects ||
        !effect_contracts_read(owner->effects)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={owner->read.work,&owner->final,NULL,owner->effects};
    return effect_invocation_result_match_source(&source,result);
}
