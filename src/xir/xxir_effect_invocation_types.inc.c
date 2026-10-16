/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_types.inc.c - Physical fields for one immutable replay
 */

/* These lists retain every actual write in ascending instruction order.
 * They index physical destinations only; Class sources are still checked
 * individually by the unchanged semantic consumer. */
static XrXirStatus effect_invocation_flow_write_index(EffectInvocationFlow *flow) {
    if (!flow || !flow->owner || !flow->function || (flow->values && !flow->write_heads) ||
        (flow->function->instruction_count && (!flow->instructions || !flow->write_next)))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=((uint64_t)flow->values+flow->function->instruction_count)*sizeof(uint32_t);
    if (!xir_compile_work(flow->owner->work,bytes)) return XR_XIR_BUDGET;
    if (flow->values) memset(flow->write_heads,0xff,(size_t)flow->values*sizeof(uint32_t));
    if (flow->function->instruction_count)
        memset(flow->write_next,0xff,(size_t)flow->function->instruction_count*sizeof(uint32_t));
    for (uint32_t i=flow->function->instruction_count;i;--i) {
        if (!xir_compile_work(flow->owner->work,3)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&flow->instructions[i-1];
        if (op->op!=XR_XIR_LOCAL_WRITE && op->op!=XR_XIR_SCALAR_LOCAL_WRITE &&
            op->op!=XR_XIR_OWNED_LOCAL_WRITE) continue;
        uint32_t destination=op->args[0];
        if (destination>=flow->values) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(flow->owner->work,1+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        flow->write_next[i-1]=flow->write_heads[destination];flow->write_heads[destination]=i-1;
    }
    return XR_XIR_OK;
}

/* Every canonical run and adjacent boundary is read in this replay. The
 * temporary rows contain no origin, effect or admission conclusion;
 * delegated readers retain their own reads and failures without a projection. */
static XrXirStatus effect_invocation_flow_types(EffectInvocationFlow *flow) {
    if (!flow || !flow->owner || !flow->function || flow->types_ready ||
        flow->node>=flow->owner->final.count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_request_sequence_owned(&flow->body)) return XR_XIR_OK;
    XrXirStatus status=effect_body_request_sequence_binding(&flow->body);
    if (status!=XR_XIR_OK) return status;
    uint32_t f=flow->owner->final.nodes[flow->node].body;
    const EffectBodyMaterial *material=flow->body.owner;
    if (material!=flow->owner->final.material || f>=effect_body_count(material) ||
        !material->functions || (flow->values && !flow->types) ||
        (flow->function->instruction_count && !flow->instructions)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&material->functions[f];
    if (body->parameter_count!=flow->function->parameter_count ||
        body->instruction_count!=flow->function->instruction_count ||
        (uint64_t)body->parameter_count+body->instruction_count!=flow->values ||
        (body->parameter_count && !body->parameters)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<body->parameter_count;++p) {
        if (!xir_compile_work(flow->owner->work,1+sizeof(*flow->types))) return XR_XIR_BUDGET;
        flow->types[p]=body->parameters[p];
    }
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    if (!xir_compile_work(flow->owner->work,sizeof(cursor))) return XR_XIR_BUDGET;
    for (uint32_t r=0;r<body->run_count;++r) {
        const EffectBodyRun *ref=NULL;
        status=effect_body_range_ref(&flow->body.read,body,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t i=ref->first;i<ref->first+ref->count;++i) {
            status=effect_body_instruction_ref_value(&flow->body.read,ref,i,&flow->instructions[i]);
            if (status!=XR_XIR_OK) return status;
            if (!xir_compile_work(flow->owner->work,1+sizeof(*flow->types))) return XR_XIR_BUDGET;
            flow->types[body->parameter_count+i]=flow->instructions[i].type;
        }
    }
    if (cursor.first!=body->instruction_count || cursor.run!=body->run_count)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_invocation_flow_write_index(flow);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(flow->owner->work,sizeof(flow->types_ready))) return XR_XIR_BUDGET;
    flow->types_ready=true;return XR_XIR_OK;
}
