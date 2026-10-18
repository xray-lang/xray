/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_parameter_coverage.inc.c - Complete actual parameter sources
 *
 * KEY CONCEPT:
 *   Each positive parameter rank has every current full-key incoming source.
 *   Original formal roots remain opaque; captures retain their real receiver.
 */
/* Each edge occurs once in its actual target's ordered incoming list. The
 * complete count prevents a missing source clause from becoming permission. */
static XrXirStatus effect_invocation_coverage_incoming_shape(const EffectInvocationRead *owner) {
    if (!owner || !owner->consumers || (owner->final->edge_count && !owner->final->edges))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t linked=0;
    for (uint32_t n=0;n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        for (uint32_t e=owner->consumers[n];e!=UINT32_MAX;e=owner->final->edges[e].consumer_next) {
            if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
            if (e>=owner->final->edge_count || ++linked>owner->final->edge_count || owner->final->edges[e].target!=n ||
                owner->final->edges[e].caller>=owner->final->count ||
                (owner->final->edges[e].consumer_next!=UINT32_MAX && owner->final->edges[e].consumer_next>=e))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return linked==owner->final->edge_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_invocation_parameter_edge(EffectInvocationFlow *source,
    uint32_t e,uint32_t parameter,uint64_t *maximum,bool *matched,bool *complete) {
    const EffectInvocationRead *owner=source->read;const EffectBodyRequest *request=&source->body;
    if (e>=owner->final->edge_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationEdge edge=owner->final->edges[e];
    if (edge.caller>=owner->final->count || edge.target>=owner->final->count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationFlow flow={.owner=source->owner,.read=owner,.node=edge.caller,.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_origins_read(&flow,request);
    EffectInvocationCoverage coverage={0};*matched=false;
    if (status==XR_XIR_OK) status=effect_invocation_coverage_input(&flow,e,matched);
    EffectInvocationProducerSource actual={UINT32_MAX,UINT32_MAX,UINT32_MAX,parameter};
    if (status==XR_XIR_OK && *matched)
        status=effect_invocation_producer_source_read(owner,request,&edge,parameter,&actual);
    if (status==XR_XIR_OK && *matched && actual.value==UINT32_MAX) *complete=false;
    if (status==XR_XIR_OK && *matched && actual.value!=UINT32_MAX) {
        status=effect_invocation_coverage_values(&flow,&coverage);
        if (status==XR_XIR_OK)
            status=effect_invocation_coverage_source(&coverage,actual.value,maximum,complete);
    }
    xr_compile_resources_free(coverage.memory);xr_compile_resources_free(flow.memory);
    return status;
}

/* Declaration roots include an external formal contribution even if a real
 * caller happens to share their key. No incoming list can discharge it. */
static XrXirStatus effect_invocation_parameter_rank(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t n,uint32_t parameter,uint64_t *output) {
    *output=0;
    if (!owner->final->roots || n>=owner->final->count || owner->final->nodes[n].body>=request->domain.function_count ||
        parameter>=owner->final->nodes[n].parameter_count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final->nodes[n];
    EffectBodyFunctionSignature head={0};
    XrXirStatus head_status=effect_body_request_signature(request,node->body,&head);
    if (head_status!=XR_XIR_OK) return head_status;
    if (parameter>=head.parameter_count || !head.parameters) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
    if (owner->final->roots[node->body]==n || !effect_physical_origin(request->domain.types,
        head.parameters[parameter])) return XR_XIR_OK;
    EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX};
    bool complete=false;
    XrXirStatus status=effect_invocation_coverage_parameter_sites(owner,request,n,parameter,&complete);
    if (status==XR_XIR_OK && !xir_compile_work(owner->work,sizeof(flow.body))) status=XR_XIR_BUDGET;
    flow.body=*request;
    if (status!=XR_XIR_OK || !complete) return status;
    uint64_t maximum=0;uint32_t count=0,walked=0;
    for (uint32_t e=owner->consumers[n];status==XR_XIR_OK && e!=UINT32_MAX;e=owner->final->edges[e].consumer_next) {
        if (!xir_compile_work(owner->work,3)) { status=XR_XIR_BUDGET;break; }
        if (e>=owner->final->edge_count || ++walked>owner->final->edge_count || owner->final->edges[e].target!=n) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        bool matched=false;
        status=effect_invocation_parameter_edge(&flow,e,parameter,&maximum,&matched,&complete);
        if (matched) ++count;
    }
    if (status==XR_XIR_OK && count && complete) {
        if (maximum==UINT64_MAX) return XR_XIR_BUDGET;
        *output=maximum+1;
    }
    return status;
}

/* Reject impossible stored authority before replaying any source clauses.
 * This is a complete shape/zero check, not a replacement for rank replay. */
static XrXirStatus effect_invocation_parameter_zero_shape(const EffectInvocationRead *owner,const EffectBodyRequest *request,uint32_t n) {
    if (!owner || !owner->final->roots || !request || !request->domain.types ||
        !owner->final->nodes || n>=owner->final->count || owner->final->nodes[n].body>=request->domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final->nodes[n];
    EffectBodyFunctionSignature head={0};
    XrXirStatus status=effect_body_request_signature(request,node->body,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionSignature *function=&head;
    if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
    uint32_t root=owner->final->roots[node->body];
    if (root>=owner->final->count || owner->final->nodes[root].body!=node->body) return XR_XIR_BAD_STRUCTURE;
    if (node->parameter_count!=function->parameter_count ||
        (!!function->parameters!=!!node->parameter_count) ||
        (!!node->parameter_coverage!=!!node->parameter_count)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<node->parameter_count;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (node->parameter_coverage[p] && (!owner->producer_enabled || root==n ||
            !effect_physical_origin(request->domain.types,function->parameters[p])))
            return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_parameter_step_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t n,bool *changed,EffectInvocationWorkspace *builder,uint64_t maximum) {
    bool verify=builder==NULL;
    if (!owner || !request || n>=owner->final->count || !changed ||
        owner->final->nodes[n].body>=request->domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionSignature head={0};
    XrXirStatus shape=effect_body_request_signature(request,owner->final->nodes[n].body,&head);
    if (shape!=XR_XIR_OK) return shape;
    if (owner->final->nodes[n].parameter_count!=head.parameter_count ||
        (!!head.parameters!=!!owner->final->nodes[n].parameter_count) ||
        (!!owner->final->nodes[n].parameter_coverage!=!!owner->final->nodes[n].parameter_count))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<owner->final->nodes[n].parameter_count;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint64_t stored=owner->final->nodes[n].parameter_coverage[p];
        if (!verify && stored) continue;
        if (verify && stored>maximum) return XR_XIR_BAD_STRUCTURE;
        uint64_t actual=0;XrXirStatus status=effect_invocation_parameter_rank(owner,request,n,p,&actual);
        if (status!=XR_XIR_OK) return status;
        if (verify) {
            if (!!stored!=!!actual || (stored && actual>stored)) return XR_XIR_BAD_STRUCTURE;
        } else if (actual) {
            builder->final.nodes[n].parameter_coverage[p]=actual;*changed=true;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_invocation_parameter_step(EffectInvocationWorkspace *owner,
    uint32_t n,bool *changed,bool verify,uint64_t maximum) {
    if (!owner || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_parameter_step_read(&owner->read,&request,n,changed,verify?NULL:owner,maximum);
}

/* Parameter sources flow forward and return sources flow back. Both wake the
 * existing equation queue; no secondary graph or recursive solver is used. */
static XrXirStatus effect_invocation_coverage_notify(EffectInvocationWorkspace *owner,uint32_t n) {
    uint32_t walked=0;
    for (uint32_t e=owner->final.nodes[n].edge_head;e!=UINT32_MAX;e=owner->final.edges[e].next) {
        if (!xir_compile_work(owner->read.work,2)) return XR_XIR_BUDGET;
        if (e>=owner->final.edge_count || ++walked>owner->final.edge_count || owner->final.edges[e].caller!=n)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_enqueue(owner,owner->final.edges[e].target);
        if (status!=XR_XIR_OK) return status;
    }
    walked=0;
    for (uint32_t e=owner->read.consumers[n];e!=UINT32_MAX;e=owner->final.edges[e].consumer_next) {
        if (!xir_compile_work(owner->read.work,2)) return XR_XIR_BUDGET;
        if (e>=owner->final.edge_count || ++walked>owner->final.edge_count || owner->final.edges[e].target!=n)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t caller=owner->final.edges[e].caller;
        if (caller>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_enqueue(owner,caller);
        if (status!=XR_XIR_OK) return status;
        /* A returned value can feed another actual argument without changing
         * its caller's own return or formals. Wake those existing targets too. */
        uint32_t outgoing=0;
        for (uint32_t sibling=owner->final.nodes[caller].edge_head;sibling!=UINT32_MAX;
             sibling=owner->final.edges[sibling].next) {
            if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
            if (sibling>=owner->final.edge_count || ++outgoing>owner->final.edge_count ||
                owner->final.edges[sibling].caller!=caller) return XR_XIR_BAD_STRUCTURE;
            status=effect_invocation_enqueue(owner,owner->final.edges[sibling].target);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
