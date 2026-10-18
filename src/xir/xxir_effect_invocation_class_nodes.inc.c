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
typedef struct EffectInvocationClassReceiver {
    EffectInvocationWorkspace *owner;
    const EffectBodyFunction *function;
    const EffectPhysicalFunction *physical;
    EffectInvocationClassFact *facts;
    uint32_t record,at;
} EffectInvocationClassReceiver;

/* Membership receives a current type before creating a fresh lattice bottom.
 * Neither physical location nor an earlier node supplies a semantic mask. */
static XrXirStatus effect_invocation_class_receive_fact(EffectInvocationClassReceiver *input,
    uint32_t value,XrXirType type) {
    if (input->at>=input->physical->classes || value>=input->physical->values ||
        !xr_xir_type_is_class(input->owner->body.domain.types,type) ||
        (input->at && input->facts[input->at-1].value>=value)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(input->owner->work,sizeof(*input->facts))) return XR_XIR_BUDGET;
    input->facts[input->at++]=(EffectInvocationClassFact){.value=value};return XR_XIR_OK;
}

/* The complete current directory is checked by the common visitor before
 * this synchronous receiver reads selected type words from each real run. */
static XrXirStatus effect_invocation_class_receive_run(void *owner,const EffectBodyRun *ref) {
    EffectInvocationClassReceiver *input=owner;EffectInvocationWorkspace *workspace=input->owner;
    if (!xir_compile_work(workspace->work,5)) return XR_XIR_BUDGET;
    uint32_t parameters=input->function->parameter_count,end=ref->first+ref->count;
    while (input->record<input->physical->count) {
        if (!xir_compile_work(workspace->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=input->physical->records[input->record];
        if (!(record.mask&EFFECT_PHYSICAL_CLASS)) { ++input->record;continue; }
        if (record.value<parameters) return XR_XIR_BAD_STRUCTURE;
        uint32_t instruction=record.value-parameters,type=XR_XIR_UNIT;
        if (instruction<ref->first) return XR_XIR_BAD_STRUCTURE;
        if (instruction>=end) return XR_XIR_OK;
        XrXirStatus status=effect_body_run_word(&workspace->body.read,ref,instruction,1,&type);
        if (status==XR_XIR_OK) status=effect_invocation_class_receive_fact(input,record.value,(XrXirType)type);
        if (status!=XR_XIR_OK) return status;
        ++input->record;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_receive_owned(EffectInvocationClassReceiver *input) {
    EffectInvocationWorkspace *owner=input->owner;
    if (!xir_compile_work(owner->work,sizeof(EffectBodyRunVisitor)+sizeof(input->record))) return XR_XIR_BUDGET;
    while (input->record<input->physical->count) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=input->physical->records[input->record];
        if (!(record.mask&EFFECT_PHYSICAL_CLASS)) { ++input->record;continue; }
        if (record.value>=input->function->parameter_count) break;
        XrXirType type=XR_XIR_UNIT;
        XrXirStatus status=effect_body_operand_type(&owner->body.read,input->function,record.value,&type);
        if (status==XR_XIR_OK) status=effect_invocation_class_receive_fact(input,record.value,type);
        if (status!=XR_XIR_OK) return status;
        ++input->record;
    }
    if (input->record==input->physical->count) return XR_XIR_OK;
    EffectBodyRunVisitor visitor={input,effect_invocation_class_receive_run};
    XrXirStatus status=effect_body_runs_visit(&owner->body.read,input->function,&visitor);
    if (status!=XR_XIR_OK) return status;
    return input->record==input->physical->count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_invocation_class_storage(EffectInvocationWorkspace *owner,
    uint32_t body,EffectInvocationClassFact **output,uint32_t *count) {
    if (!output || *output || !count || *count || body>=owner->body.domain.function_count ||
        !owner->final.physical || owner->final.physical->resources!=owner->work->resources ||
        owner->final.physical->count!=owner->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_invocation_dimensions(owner,body,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionDimensions *function=&head;
    const EffectPhysicalFunction *physical=&owner->final.physical->functions[body];
    if (physical->values!=(uint64_t)function->parameter_count+function->instruction_count ||
        physical->classes>physical->values || physical->count>physical->capacity ||
        (physical->count && !physical->records)) return XR_XIR_BAD_STRUCTURE;
    if (!physical->classes) return XR_XIR_OK;
    bool native=effect_body_request_sequence_owned(&owner->body);
    if (native) {
        status=effect_body_request_sequence_binding(&owner->body);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (owner->body.owner!=owner->final.material || owner->final.material->physical!=owner->final.physical)
            return XR_XIR_BAD_STRUCTURE;
    }
    EffectInvocationClassFact *facts=xir_compile_calloc(owner->work,
        physical->classes,sizeof(*facts),&status);
    if (!facts) return status;
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationClassReceiver))) {
        xr_compile_resources_free(facts);return XR_XIR_BUDGET;
    }
    EffectInvocationClassReceiver input={owner,native?&owner->final.material->functions[body]:NULL,physical,facts,0,0};
    if (native) status=effect_invocation_class_receive_owned(&input);
    for (uint32_t r=0;!native && r<physical->count && status==XR_XIR_OK;++r) {
        if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
        EffectPhysicalRecord record=physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_CLASS)) continue;
        XrXirType type;status=effect_body_request_type(&owner->body,body,record.value,&type);
        if (status!=XR_XIR_OK) break;
        status=effect_invocation_class_receive_fact(&input,record.value,type);
    }
    if (status==XR_XIR_OK && input.at!=physical->classes) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(facts);return status; }
    *output=facts;*count=input.at;return XR_XIR_OK;
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
static XrXirStatus effect_invocation_class_rank_limit_count(EffectInvocationWorkspace *owner,
    uint32_t functions,uint64_t *output) {
    if (!output || !owner->final.physical || owner->final.physical->count!=functions)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t maximum=0;
    for (uint32_t n=0;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final.nodes[n];
        if (node->body>=owner->final.physical->count ||
            node->class_count!=owner->final.physical->functions[node->body].classes ||
            (uint64_t)node->class_count+1>UINT64_MAX-maximum) return XR_XIR_BAD_STRUCTURE;
        maximum+=(uint64_t)node->class_count+1;
    }
    *output=maximum;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_rank_limit(EffectInvocationWorkspace *owner,
    uint64_t *output) {
    if (!owner || !effect_body_material_shape(owner->final.material)) return XR_XIR_BAD_STRUCTURE;
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
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    uint64_t maximum=owner->class_rank_limit;
    for (uint32_t n=owner->class_rank_nodes;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const EffectInvocationNode *node=&owner->final.nodes[n];
        if (node->body>=functions || node->class_count!=owner->final.physical->functions[node->body].classes ||
            (uint64_t)node->class_count+1>UINT64_MAX-maximum) return XR_XIR_BAD_STRUCTURE;
        maximum+=(uint64_t)node->class_count+1;
    }
    if (owner->class_rank_nodes!=owner->final.count) {
        if (!xir_compile_work(owner->work,sizeof(owner->class_rank_limit)+sizeof(owner->class_rank_nodes)))
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
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    uint32_t joined=destination->mask|mask;
    /* A positive rank certifies a complete pure source. Mixed residuals
     * have no such rank; an earlier complete proof can only be shortened. */
    uint64_t distance=joined==EFFECT_CLASS_KNOWN ?
        (!destination->rank || rank<destination->rank ? rank : destination->rank) : 0;
    if (destination->mask!=joined || destination->rank!=distance) {
        if (!xir_compile_work(owner->work,sizeof(destination->mask)+sizeof(destination->rank)))
            return XR_XIR_BUDGET;
        destination->mask=joined;destination->rank=distance;*changed=true;
    }
    return XR_XIR_OK;
}
