/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_iterator_parameters.inc.c - Current immutable formal consumption rows
 */
/* This is a representation choice inside one synchronous proof. A local write,
 * SSA receiver or pending call retains the complete origin-flow reader. */
static XrXirStatus effect_invocation_iterator_parameters_select(EffectInvocationFlow *flow,bool *output) {
    *output=false;bool found=false;
    for (uint32_t r=0;r<flow->physical->count;++r) {
        if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        const EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&(EFFECT_PHYSICAL_TRANSFER|EFFECT_PHYSICAL_INVOCATION))) continue;
        if (record.value<flow->function->parameter_count || record.value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        uint32_t i=record.value-flow->function->parameter_count;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus status=effect_invocation_flow_instruction(flow,i,&storage,&op);
        if (status!=XR_XIR_OK) return status;
        if (effect_invocation_iterator_operation(op->op)) {
            if (!(record.mask&EFFECT_PHYSICAL_ITERATOR_ADVANCE)) return XR_XIR_BAD_STRUCTURE;
            if (op->args[0]>=flow->function->parameter_count) return XR_XIR_OK;
            XrXirType type=0;status=effect_invocation_flow_type(flow,op->args[0],&type);
            if (status!=XR_XIR_OK) return status;
            if (!effect_physical_iterator(flow->body.domain.types,type)) return XR_XIR_BAD_TYPE;
            found=true;continue;
        }
        if ((op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_SCALAR_LOCAL_WRITE ||
            op->op==XR_XIR_OWNED_LOCAL_WRITE || op->op==XR_XIR_CELL_LOCAL_WRITE) &&
            op->args[0]<flow->function->parameter_count) return XR_XIR_OK;
        bool pending=false;
        status=effect_invocation_iterator_call_candidate(flow->read,&flow->body,
            flow->read->final->nodes[flow->node].body,op,&pending);
        if (status!=XR_XIR_OK) return status;
        if (pending) return XR_XIR_OK;
    }
    *output=found;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_iterator_parameters_row(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,uint64_t *row,uint32_t *matches) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationNode *node=&owner->final->nodes[flow->node];uint32_t p=op->args[0];
    if (!effect_invocation_iterator_operation(op->op) || p>=node->parameter_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,(uint64_t)flow->basis.fn_words*sizeof(*row))) return XR_XIR_BUDGET;
    memcpy(row,node->input+(size_t)p*flow->basis.row_words,(size_t)flow->basis.fn_words*sizeof(*row));
    bool complete=false;
    XrXirStatus status=effect_invocation_coverage_sites_row(flow,p,row,true,&complete);
    uint64_t rank=complete?node->parameter_coverage[p]:0;uint32_t mask=0;bool deferred=false;
    if (status==XR_XIR_OK) status=effect_invocation_iterator_obligation_row(flow,row,rank,&mask,&deferred);
    const EffectInvocationClosureView current={p,row};
    if (status==XR_XIR_OK) status=effect_invocation_iterator_edges_read(flow,instruction,op,&current,matches);
    if (status==XR_XIR_OK && flow->owner) status=effect_invocation_iterator_record(flow,instruction,mask,deferred);
    else if (status==XR_XIR_OK) {
        bool stored=(effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true)[instruction/64]&
            (UINT64_C(1)<<(instruction%64)))!=0;
        const XrXirRootEffectWitness *terminal=&effect_invocation_local_causes(node)[1].witness;
        if ((mask&~node->local_mask) || stored!=deferred || (!mask &&
            terminal->cause==XR_XIR_ROOT_CAUSE_INDIRECT && terminal->instruction==instruction)) status=XR_XIR_BAD_STRUCTURE;
    }
    return status;
}

/* The copied formal row uses the same site, rank, argument and edge readers as
 * full SSA replay. It owns only this request's words and no saved verdict. */
static XrXirStatus effect_invocation_iterator_parameters_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,EffectInvocationWorkspace *builder,uint32_t n,bool *handled) {
    *handled=false;
    if (!effect_body_request_owned_matches(owner->work,request,owner->final->material)) return XR_XIR_OK;
    const EffectInvocationNode *node=&owner->final->nodes[n];
    const EffectPhysicalDomain *domain=owner->final->physical;
    if (!domain || !domain->functions || domain->resources!=owner->work->resources ||
        node->body>=domain->count) return XR_XIR_BAD_STRUCTURE;
    const EffectPhysicalFunction *physical=&domain->functions[node->body];
    if (physical->count>physical->capacity || (physical->count && !physical->records))
        return XR_XIR_BAD_STRUCTURE;
    if (!physical->parameters) return XR_XIR_OK;
    bool advance=false;
    for (uint32_t r=0;r<physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        advance|=(physical->records[r].mask&EFFECT_PHYSICAL_ITERATOR_ADVANCE)!=0;
    }
    if (!advance) return XR_XIR_OK;
    EffectInvocationFlow flow={.read=owner,.owner=builder,.node=n,.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_formal_prepare(&flow,request);bool selected=false;
    if (status==XR_XIR_OK) status=effect_invocation_iterator_parameters_select(&flow,&selected);
    if (status!=XR_XIR_OK || !selected) return status;
    uint64_t row_bytes=(uint64_t)flow.basis.fn_words*sizeof(uint64_t);
    uint64_t match_bytes=(uint64_t)owner->final->site_count*sizeof(uint32_t);
    if (row_bytes>SIZE_MAX || match_bytes>SIZE_MAX-row_bytes) return XR_XIR_BUDGET;
    void *memory=xir_compile_alloc(owner->work,(size_t)(row_bytes+match_bytes),&status);
    if (!memory) return status;
    uint64_t *row=memory;uint32_t *matches=(uint32_t *)((uint8_t *)memory+(size_t)row_bytes);
    for (uint32_t r=0;status==XR_XIR_OK && r<physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) {status=XR_XIR_BUDGET;break;}
        const EffectPhysicalRecord record=physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_ITERATOR_ADVANCE)) continue;
        if (record.value<flow.function->parameter_count) {status=XR_XIR_BAD_STRUCTURE;break;}
        uint32_t i=record.value-flow.function->parameter_count;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_invocation_flow_instruction(&flow,i,&storage,&op);
        if (status==XR_XIR_OK) status=effect_invocation_iterator_parameters_row(&flow,i,op,row,matches);
    }
    xr_compile_resources_free(memory);if (status==XR_XIR_OK) *handled=true;return status;
}
