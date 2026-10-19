/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_coverage_check.inc.c - Single-request complete source clauses
 */
typedef struct EffectInvocationCoverageClause {
    uint64_t maximum;
    uint32_t matched;
    bool eligible,complete;
} EffectInvocationCoverageClause;
typedef struct EffectInvocationCoverageCheck {
    EffectInvocationCoverageClause *clauses;
    uint32_t *offsets;
    void *memory;
    uint64_t maximum;
    uint32_t count;
} EffectInvocationCoverageCheck;

/* Offsets describe only this proof's actual formals. No computed rank or
 * verdict is retained by the Final, builder, Source or runtime consumer. */
static XrXirStatus effect_invocation_coverage_check_begin(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,EffectInvocationCoverageCheck *check) {
    if (!owner || !owner->final || !request || !check || check->memory ||
        !owner->producer_enabled || !owner->final->count || !request->domain.function_count ||
        !owner->final->roots || !owner->final->nodes ||
        !effect_body_request_owned_matches(owner->work,request,owner->final->material)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_invocation_coverage_incoming_shape(owner);uint64_t parameters=0,maximum=0;
    if (status!=XR_XIR_OK) return status;
    uint32_t count=owner->final->count;const EffectInvocationNode *nodes=owner->final->nodes;
    for (uint32_t n=0;n<count;++n) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        status=effect_invocation_parameter_zero_shape(owner,request,n);
        if (status!=XR_XIR_OK) return status;
        EffectBodyFunctionDimensions head={0};status=effect_body_request_dimensions(request,nodes[n].body,&head);
        if (status!=XR_XIR_OK) return status;
        uint64_t vertices=(uint64_t)head.parameter_count+head.instruction_count+1;
        if (vertices>UINT64_MAX-maximum || head.parameter_count>UINT32_MAX-parameters) return XR_XIR_BUDGET;
        maximum+=vertices;parameters+=head.parameter_count;
    }
    uint64_t clause_bytes=parameters*sizeof(*check->clauses),offset_bytes=((uint64_t)count+1)*sizeof(*check->offsets);
    if (clause_bytes>SIZE_MAX || offset_bytes>SIZE_MAX-clause_bytes) return XR_XIR_BUDGET;
    void *memory=xir_compile_calloc(owner->work,1,(size_t)(clause_bytes+offset_bytes),&status);
    if (!memory) return status;
    check->memory=memory;check->clauses=memory;
    check->offsets=(uint32_t *)((uint8_t *)memory+(size_t)clause_bytes);check->count=count;check->maximum=maximum;
    if (!xir_compile_work(owner->work,sizeof(*check)+2)) return XR_XIR_BUDGET;
    if (owner->final->count!=count || owner->final->nodes!=nodes) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;
    for (uint32_t n=0;n<count;++n) {
        EffectBodyFunctionSignature head={0};status=effect_body_request_signature(request,nodes[n].body,&head);
        if (status!=XR_XIR_OK) return status;
        if (nodes[n].parameter_count!=head.parameter_count || head.parameter_count>parameters-at ||
            (head.parameter_count && (!head.parameters || !nodes[n].input))) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,sizeof(*check->offsets))) return XR_XIR_BUDGET;
        check->offsets[n]=at;
        for (uint32_t p=0;p<head.parameter_count;++p,++at) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            if (owner->final->roots[nodes[n].body]==n || !effect_physical_origin(request->domain.types,head.parameters[p])) continue;
            check->clauses[at].eligible=true;check->clauses[at].complete=true;
        }
    }
    if (at!=parameters) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,sizeof(*check->offsets))) return XR_XIR_BUDGET;
    check->offsets[count]=at;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_coverage_check_edge(EffectInvocationFlow *flow,uint32_t e,
    EffectInvocationCoverage *coverage,EffectInvocationCoverageCheck *check) {
    const EffectInvocationRead *owner=flow->read;
    if (e>=owner->final->edge_count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationEdge edge=owner->final->edges[e];
    if (edge.target>=check->count || edge.caller!=flow->node) return XR_XIR_BAD_STRUCTURE;
    uint32_t first=check->offsets[edge.target],count=check->offsets[edge.target+1]-first;
    if (count!=owner->final->nodes[edge.target].parameter_count) return XR_XIR_BAD_STRUCTURE;
    bool eligible=false;
    for (uint32_t p=0;p<count;++p) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!check->clauses[first+p].eligible) continue;
        bool complete=false;XrXirStatus status=effect_invocation_coverage_parameter_sites(owner,&flow->body,edge.target,p,&complete);
        if (status!=XR_XIR_OK) return status;
        if (complete) {eligible=true;break;}
    }
    if (!eligible) return XR_XIR_OK;
    bool matched=false;XrXirStatus status=effect_invocation_coverage_input(flow,e,&matched);
    if (status!=XR_XIR_OK || !matched) return status;
    for (uint32_t p=0;p<count;++p) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        EffectInvocationCoverageClause *clause=&check->clauses[first+p];
        if (!clause->eligible) continue;
        bool complete=false;status=effect_invocation_coverage_parameter_sites(owner,&flow->body,edge.target,p,&complete);
        if (status!=XR_XIR_OK) return status;
        if (!complete) continue;
        if (clause->matched==UINT32_MAX) return XR_XIR_BUDGET;
        ++clause->matched;EffectInvocationProducerSource actual={UINT32_MAX,UINT32_MAX,UINT32_MAX,p};
        status=effect_invocation_producer_source_read(owner,&flow->body,&edge,p,&actual);
        if (status!=XR_XIR_OK) return status;
        if (actual.value==UINT32_MAX) {clause->complete=false;continue;}
        if (!coverage->memory) status=effect_invocation_coverage_values(flow,coverage);
        if (status==XR_XIR_OK) status=effect_invocation_coverage_source(coverage,actual.value,&clause->maximum,&clause->complete);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Every actual RETURN remains a separate source clause. The same complete
 * caller relation supplies its ranks and every outgoing parameter source. */
static XrXirStatus effect_invocation_coverage_check_return(EffectInvocationFlow *flow,
    EffectInvocationCoverage *coverage,const EffectInvocationCoverageCheck *check) {
    const EffectInvocationRead *owner=flow->read;bool supported=false;
    XrXirStatus status=effect_invocation_producer_return_read(owner,&flow->body,owner->final->nodes[flow->node].body,&supported);
    if (status!=XR_XIR_OK) return status;
    uint64_t stored=owner->final->nodes[flow->node].return_coverage;
    if (stored>check->maximum) return XR_XIR_BAD_STRUCTURE;
    if (!supported) return stored?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    bool present=false,complete=true;uint64_t maximum=0;
    for (uint32_t r=0;r<flow->physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        const EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_RETURN)) continue;
        if (record.value<flow->function->parameter_count || record.value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        status=effect_invocation_flow_instruction(flow,record.value-flow->function->parameter_count,&storage,&op);
        if (status!=XR_XIR_OK) return status;
        if (op->op!=XR_XIR_RETURN) return XR_XIR_BAD_STRUCTURE;
        if (!coverage->memory) status=effect_invocation_coverage_values(flow,coverage);
        if (status==XR_XIR_OK) status=effect_invocation_coverage_source(coverage,op->args[0],&maximum,&complete);
        if (status!=XR_XIR_OK) return status;present=true;
    }
    if (maximum==UINT64_MAX) return XR_XIR_BUDGET;
    uint64_t actual=present && complete?maximum+1:0;
    return !!stored!=!!actual || (stored && actual>stored)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
}

static XrXirStatus effect_invocation_coverage_check_visit(EffectInvocationFlow *flow,
    EffectInvocationCoverageCheck *check) {
    EffectInvocationCoverage coverage={0};XrXirStatus status=XR_XIR_OK;uint32_t walked=0;
    for (uint32_t e=flow->read->final->nodes[flow->node].edge_head;e!=UINT32_MAX;e=flow->read->final->edges[e].next) {
        if (!xir_compile_work(flow->read->work,2)) {status=XR_XIR_BUDGET;break;}
        if (e>=flow->read->final->edge_count || ++walked>flow->read->final->edge_count) {status=XR_XIR_BAD_STRUCTURE;break;}
        status=effect_invocation_coverage_check_edge(flow,e,&coverage,check);
        if (status!=XR_XIR_OK) break;
    }
    if (status==XR_XIR_OK) status=effect_invocation_coverage_check_return(flow,&coverage,check);
    xr_compile_resources_free(coverage.memory);return status;
}

static XrXirStatus effect_invocation_coverage_check_finish(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,const EffectInvocationCoverageCheck *check) {
    if (owner->final->count!=check->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<check->count;++n) {
        const EffectInvocationNode *node=&owner->final->nodes[n];uint32_t first=check->offsets[n];
        if (node->parameter_count!=check->offsets[n+1]-first || (node->parameter_count && !node->parameter_coverage))
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p=0;p<node->parameter_count;++p) {
            if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
            const EffectInvocationCoverageClause *clause=&check->clauses[first+p];
            if (clause->maximum==UINT64_MAX) return XR_XIR_BUDGET;
            bool complete=false;
            if (clause->eligible) {
                XrXirStatus status=effect_invocation_coverage_parameter_sites(owner,request,n,p,&complete);
                if (status!=XR_XIR_OK) return status;
            }
            uint64_t actual=complete && clause->matched && clause->complete?clause->maximum+1:0;
            uint64_t stored=node->parameter_coverage[p];
            if (stored>check->maximum || !!stored!=!!actual || (stored && actual>stored)) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
