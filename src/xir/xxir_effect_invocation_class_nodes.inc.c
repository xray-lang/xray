/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_class_nodes.inc.c - Physical Class rows on the one equation owner
 *
 * Complete physical membership is copied into each genuine equation key.
 * These storage facts do not authorize a Class projection or erase UNKNOWN.
 */
static XrXirStatus effect_invocation_class_storage(EffectInvocationWorkspace *owner,
    uint32_t body,EffectInvocationClassFact **output,uint32_t *count) {
    if (!output || *output || !count || *count || body>=owner->read.body.domain.function_count ||
        !owner->final.physical || owner->final.physical->resources!=owner->read.work->resources ||
        owner->final.physical->count!=owner->read.body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_invocation_dimensions(&owner->read,body,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionDimensions *function=&head;
    const EffectPhysicalFunction *physical=&owner->final.physical->functions[body];
    if (physical->values!=(uint64_t)function->parameter_count+function->instruction_count ||
        physical->classes>physical->values || physical->count>physical->capacity ||
        (physical->count && !physical->records)) return XR_XIR_BAD_STRUCTURE;
    if (!physical->classes) return XR_XIR_OK;
    EffectInvocationClassFact *facts=xir_compile_calloc(owner->read.work,
        physical->classes,sizeof(*facts),&status);
    if (!facts) return status;
    uint32_t at=0;
    for (uint32_t r=0;r<physical->count && status==XR_XIR_OK;++r) {
        if (!xir_compile_work(owner->read.work,1)) { status=XR_XIR_BUDGET;break; }
        EffectPhysicalRecord record=physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_CLASS)) continue;
        XrXirType type;status=effect_body_request_type(&owner->read.body,body,record.value,&type);
        if (status!=XR_XIR_OK) break;
        if (at>=physical->classes || record.value>=physical->values ||
            !xr_xir_type_is_class(owner->read.body.domain.types,type) ||
            (at && facts[at-1].value>=record.value)) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!xir_compile_work(owner->read.work,sizeof(*facts))) { status=XR_XIR_BUDGET;break; }
        facts[at++]=(EffectInvocationClassFact){.value=record.value};
    }
    if (status==XR_XIR_OK && at!=physical->classes) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(facts);return status; }
    *output=facts;*count=at;return XR_XIR_OK;
}

/* A missing Class row is a malformed physical relation, never a None fact.
 * Its lookup belongs to this exact body/input equation and is fully charged. */
static XrXirStatus effect_invocation_class_find(const XrXirCompileContext *work,
    const EffectInvocationNode *node,uint32_t value,uint32_t *output) {
    if (!output || !node || (node->class_count && !node->classes)) return XR_XIR_BAD_STRUCTURE;
    uint32_t low=0,high=node->class_count;
    while (low<high) {
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        uint32_t middle=low+(high-low)/2;
        if (node->classes[middle].value<value) low=middle+1;
        else high=middle;
    }
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    if (low>=node->class_count || node->classes[low].value!=value) return XR_XIR_BAD_STRUCTURE;
    *output=low;return XR_XIR_OK;
}

/* The true full graph bounds a descending certificate. Growing the existing
 * owner updates this bound; an unrooted cycle cannot certify itself at rank0. */
static XrXirStatus effect_invocation_class_rank_limit_count(const EffectInvocationRead *owner,
    uint32_t functions,uint64_t *output) {
    if (!output || !owner->final->physical || owner->final->physical->count!=functions)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t maximum=0;
    for (uint32_t n=0;n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final->nodes[n];
        if (node->body>=owner->final->physical->count ||
            node->class_count!=owner->final->physical->functions[node->body].classes ||
            (uint64_t)node->class_count+1>UINT64_MAX-maximum) return XR_XIR_BAD_STRUCTURE;
        maximum+=(uint64_t)node->class_count+1;
    }
    *output=maximum;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_rank_limit(const EffectInvocationRead *owner,
    uint64_t *output) {
    if (!owner || !effect_body_material_shape(owner->final->material)) return XR_XIR_BAD_STRUCTURE;
    return effect_invocation_class_rank_limit_count(owner,owner->body.domain.function_count,output);
}

/* Node membership and Class row counts never change after intern. Only this
 * private construction workspace remembers the already validated prefix.
 * Independent receiving and sealing still recount the complete graph. */
static XrXirStatus effect_invocation_class_rank_current(EffectInvocationWorkspace *owner,
    uint32_t functions,uint64_t *output) {
    if (!owner || !output || !owner->final.physical ||
        owner->final.physical->count!=functions || (functions && !owner->final.physical->functions) ||
        owner->class_rank_nodes>owner->final.count ||
        (owner->final.count && !owner->final.nodes)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,5)) return XR_XIR_BUDGET;
    uint64_t maximum=owner->class_rank_limit;
    for (uint32_t n=owner->class_rank_nodes;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final.nodes[n];
        if (node->body>=functions || node->class_count!=owner->final.physical->functions[node->body].classes ||
            (uint64_t)node->class_count+1>UINT64_MAX-maximum) return XR_XIR_BAD_STRUCTURE;
        maximum+=(uint64_t)node->class_count+1;
    }
    if (owner->class_rank_nodes!=owner->final.count) {
        if (!xir_compile_work(owner->read.work,sizeof(owner->class_rank_limit)+sizeof(owner->class_rank_nodes)))
            return XR_XIR_BUDGET;
        owner->class_rank_limit=maximum;owner->class_rank_nodes=owner->final.count;
    }
    *output=maximum;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_fact_join(EffectInvocationWorkspace *owner,
    EffectInvocationClassFact *destination,uint32_t mask,uint64_t rank,
    uint64_t maximum,bool *changed) {
    if (!destination || !changed || (mask&~EFFECT_CLASS_VALID) || rank>maximum ||
        (mask && !(mask&EFFECT_CLASS_KNOWN)) ||
        (mask==EFFECT_CLASS_KNOWN ? !rank : rank!=0)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,5)) return XR_XIR_BUDGET;
    uint32_t joined=destination->mask|mask;
    /* A positive rank certifies a complete pure source. Mixed residuals
     * have no such rank; an earlier complete proof can only be shortened. */
    uint64_t distance=joined==EFFECT_CLASS_KNOWN ?
        (!destination->rank || rank<destination->rank ? rank : destination->rank) : 0;
    if (destination->mask!=joined || destination->rank!=distance) {
        if (!xir_compile_work(owner->read.work,sizeof(destination->mask)+sizeof(destination->rank)))
            return XR_XIR_BUDGET;
        destination->mask=joined;destination->rank=distance;*changed=true;
    }
    return XR_XIR_OK;
}
