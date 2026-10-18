/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_iterator_verify.inc.c - Complete Iterator consumption proof
 */
/* A formal Iterator retains its actual consumption frontier. An opaque or
 * unrooted source is UNKNOWN even when its temporary target set is empty. */
static XrXirStatus effect_invocation_iterator_obligation_row(EffectInvocationFlow *flow,
    const uint64_t *row,bool complete,uint32_t *mask,bool *deferred) {
    if (!flow || !row || !mask || !deferred) return XR_XIR_BAD_STRUCTURE;
    uint32_t opaque=flow->read->final->site_count+flow->basis.parameters;
    bool symbolic=false;*mask=0;*deferred=false;
    for (uint32_t p=0;p<flow->basis.parameters;++p) {
        if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        uint32_t bit=flow->read->final->site_count+p;
        if (row[bit/64]&(UINT64_C(1)<<(bit%64))) symbolic=true;
    }
    if (!xir_compile_work(flow->read->work,4)) return XR_XIR_BUDGET;
    if ((row[opaque/64]&(UINT64_C(1)<<(opaque%64))) ||
        (!complete && !symbolic)) *mask=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    *deferred=symbolic;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_iterator_obligation(EffectInvocationFlow *flow,
    const XrXirInstruction *op,uint32_t *mask,bool *deferred) {
    if (!flow || !op || !mask || !deferred || !effect_invocation_iterator_operation(op->op) ||
        op->args[0]>=flow->values) return XR_XIR_BAD_STRUCTURE;
    bool complete=false;
    XrXirStatus status=effect_final_callable_complete(flow->read->work,flow->read->final,
        flow->node,op->args[0],&complete);
    return status==XR_XIR_OK?effect_invocation_iterator_obligation_row(flow,
        effect_invocation_row(flow,op->args[0]),complete,mask,deferred):status;
}

/* Every currently possible producer needs exactly one complete input edge.
 * Historical smaller keys remain harmless graph members, never substitutes
 * for the current SSA relation at this actual consumption instruction. */
static XrXirStatus effect_invocation_iterator_edges_read(EffectInvocationFlow *flow,
    uint32_t instruction,const XrXirInstruction *op,const EffectInvocationClosureView *supplied,uint32_t *matches) {
    const EffectInvocationRead *owner=flow->read;
    if (!xir_compile_work(owner->work,(uint64_t)owner->final->site_count*sizeof(*matches)))
        return XR_XIR_BUDGET;
    if (owner->final->site_count) memset(matches,0,(size_t)owner->final->site_count*sizeof(*matches));
    uint32_t walked=0;
    for (uint32_t e=owner->final->nodes[flow->node].edge_head;e!=UINT32_MAX;e=owner->final->edges[e].next) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (e>=owner->final->edge_count || ++walked>owner->final->edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge edge=owner->final->edges[e];
        if (edge.instruction!=instruction) continue;
        if (edge.latent || edge.producer>=owner->final->site_count) return XR_XIR_BAD_STRUCTURE;
        bool matched=false;XrXirStatus status=effect_invocation_arguments_match(flow,e,supplied,&matched);
        if (status!=XR_XIR_OK) return status;
        if (matched && ++matches[edge.producer]!=1) return XR_XIR_BAD_STRUCTURE;
    }
    const uint64_t *row=supplied?supplied->row:effect_invocation_row(flow,op->args[0]);
    if (!row) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t s=0;s<owner->final->site_count;++s) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!!(row[s/64]&(UINT64_C(1)<<(s%64)))!=(matches[s]==1)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}



/* Residuals alter no identity or input key. Their local terminals are derived
 * only after the sole discovery queue and positive coverage have closed. */
static XrXirStatus effect_invocation_iterator_record(EffectInvocationFlow *flow,
    uint32_t instruction,uint32_t mask,bool deferred) {
    EffectInvocationWorkspace *owner=flow->owner;
    if (!owner || flow->read!=&owner->read || flow->node>=owner->final.count)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&owner->final.nodes[flow->node];bool changed=false;
    if (!xir_compile_work(flow->read->work,5)) return XR_XIR_BUDGET;
    if ((mask&~node->local_mask) || (deferred && !(effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true)
        [instruction/64]&(UINT64_C(1)<<(instruction%64))))) changed=true;
    node->local_mask|=mask;node->intrinsic_mask|=mask;
    if (deferred) {
        node->local_deferred=true;node->deferred=true;
        effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true)[instruction/64]|=UINT64_C(1)<<(instruction%64);
        effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false)[instruction/64]|=UINT64_C(1)<<(instruction%64);
    }
    for (uint32_t kind=0;kind<2;++kind) {
        if (kind?!deferred:!mask) continue;
        uint32_t atom=kind?flow->basis.parameters*2+2:1;
        XrXirRootEffectWitness witness={kind?XR_XIR_ROOT_CAUSE_CONTEXT_CALL:XR_XIR_ROOT_CAUSE_INDIRECT,
            instruction,UINT32_MAX,UINT32_MAX,0};
        EffectInvocationCause *local=&effect_invocation_local_causes(node)[atom];
        if (local->witness.cause && !effect_root_cause_before(&witness,&local->witness)) continue;
        if (!xir_compile_work(flow->read->work,2*sizeof(*local))) return XR_XIR_BUDGET;
        *local=(EffectInvocationCause){witness,UINT32_MAX,UINT32_MAX,0};node->causes[atom]=*local;
    }
    if (!changed) return XR_XIR_OK;
    for (uint32_t e=owner->read.consumers[flow->node];e!=UINT32_MAX;e=owner->final.edges[e].consumer_next) {
        if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        if (e>=owner->final.edge_count || owner->final.edges[e].target!=flow->node) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_enqueue(owner,owner->final.edges[e].caller);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* An actual opcode and its typed operand span establish whether this body
 * has a pending Iterator input at all. Absence needs no origin workspace. */
static XrXirStatus effect_invocation_iterator_call_candidate(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t body,const XrXirInstruction *op,bool *output) {
    *output=false;
    if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE &&
        op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) return XR_XIR_OK;
    if (op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_INVOKE_INDIRECT) {*output=true;return XR_XIR_OK;}
    if (!op->args[1]) return XR_XIR_OK;
    EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(request,body,&head);
    if (status!=XR_XIR_OK) return status;
    if (op->args[0]>head.operand_count || op->args[1]>head.operand_count-op->args[0] ||
        !head.operands) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<op->args[1];++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        XrXirType type=0;status=effect_body_request_type(request,body,head.operands[op->args[0]+p],&type);
        if (status!=XR_XIR_OK) return status;
        if (effect_physical_iterator(request->domain.types,type)) *output=true;
    }
    return XR_XIR_OK;
}

#include "xxir_effect_invocation_iterator_parameters.inc.c"

static XrXirStatus effect_invocation_iterator_presence(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t body,bool *present,bool *advances) {
    *present=false;*advances=false;
    if (!owner->final->physical || body>=owner->final->physical->count ||
        !owner->final->physical->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectPhysicalFunction *physical=&owner->final->physical->functions[body];
    if (physical->count>physical->capacity || (physical->count && !physical->records)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t r=0;r<physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        uint32_t mask=physical->records[r].mask;
        *advances|=(mask&EFFECT_PHYSICAL_ITERATOR_ADVANCE)!=0;
        if ((mask&EFFECT_PHYSICAL_ITERATOR_ADVANCE)!=0) {*present=true;continue;}
        if (*present || physical->origins==physical->rows || !(mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        if (physical->records[r].value<physical->parameters) return XR_XIR_BAD_STRUCTURE;
        uint32_t i=physical->records[r].value-physical->parameters;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus status=effect_body_request_operation(request,body,i,&storage,&op);
        if (status==XR_XIR_OK) status=effect_invocation_iterator_call_candidate(owner,request,body,op,present);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_iterator_flow_read(EffectInvocationFlow *flow,bool advances) {
    const EffectInvocationRead *owner=flow->read;const EffectPhysicalFunction *physical=flow->physical;
    XrXirStatus status=XR_XIR_OK;uint32_t *matches=NULL;
    if (advances && owner->final->site_count) {
        uint64_t bytes=(uint64_t)owner->final->site_count*sizeof(*matches);
        if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
        matches=xir_compile_alloc(owner->work,(size_t)bytes,&status);
        if (!matches) return status;
    }
        for (uint32_t r=0;status==XR_XIR_OK && r<physical->count;++r) {
            if (!xir_compile_work(owner->work,1)) {status=XR_XIR_BUDGET;break;}
            const EffectPhysicalRecord record=physical->records[r];
            bool advance=(record.mask&EFFECT_PHYSICAL_ITERATOR_ADVANCE)!=0;
            if (!advance && !(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
            if (record.value<flow->function->parameter_count) {status=XR_XIR_BAD_STRUCTURE;break;}
            uint32_t i=record.value-flow->function->parameter_count;
            XrXirInstruction storage;const XrXirInstruction *op=NULL;
            status=effect_invocation_flow_instruction(flow,i,&storage,&op);
            uint32_t mask=0;bool deferred=false;
            if (status==XR_XIR_OK && advance)
                status=effect_invocation_iterator_obligation(flow,op,&mask,&deferred);
            else if (status==XR_XIR_OK) status=effect_invocation_iterator_pending(flow,op,&deferred);
            if (status==XR_XIR_OK && !advance && !deferred) continue;
            if (status==XR_XIR_OK && advance) status=effect_invocation_iterator_edges_read(flow,i,op,NULL,matches);
            if (status==XR_XIR_OK && flow->owner) status=effect_invocation_iterator_record(flow,i,mask,deferred);
            else if (status==XR_XIR_OK) {
                const EffectInvocationNode *node=&owner->final->nodes[flow->node];
                bool stored=(effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,true)[i/64]&
                    (UINT64_C(1)<<(i%64)))!=0;
                const XrXirRootEffectWitness *terminal=&effect_invocation_local_causes(node)[1].witness;
                if ((mask&~node->local_mask) || stored!=deferred || (!mask &&
                    terminal->cause==XR_XIR_ROOT_CAUSE_INDIRECT && terminal->instruction==i)) status=XR_XIR_BAD_STRUCTURE;
            }
        }
    xr_compile_resources_free(matches);return status;
}

/* One synchronous owned replay may supply its complete origin relation to
 * another proof. Its producer, edge, rank and terminal clauses remain real
 * independent reads; no row or verdict survives the current request. */


static XrXirStatus effect_invocation_iterators_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,EffectInvocationWorkspace *builder) {
    if (!owner || !request || !owner->final->physical || !owner->final->nodes) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<owner->final->count;++n) {
        bool formal=false;XrXirStatus status=effect_invocation_iterator_parameters_read(owner,request,builder,n,&formal);
        if (status!=XR_XIR_OK) return status;
        if (formal) continue;
        bool present=false,advances=false;
        status=effect_invocation_iterator_presence(owner,request,owner->final->nodes[n].body,&present,&advances);
        if (status!=XR_XIR_OK) return status;
        if (!present) continue;
        EffectInvocationFlow flow={.owner=builder,.read=owner,.node=n,.instruction=UINT32_MAX};
        status=effect_invocation_origins_read(&flow,request);
        if (status==XR_XIR_OK) status=effect_invocation_iterator_flow_read(&flow,advances);
        xr_compile_resources_free(flow.memory);if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_iterator_residual(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_iterators_read(&owner->read,&request,owner);
}
