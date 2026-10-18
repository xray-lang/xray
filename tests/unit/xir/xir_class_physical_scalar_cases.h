/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_physical_scalar_cases.h - Frozen scalar Class comparison
 *
 * KEY CONCEPT:
 *   Complete physical membership, every source and every return are replayed.
 *   A zero rank, open formal or unknown carrier cannot prove a pure identity.
 */
#ifndef XIR_CLASS_PHYSICAL_SCALAR_CASES_H
#define XIR_CLASS_PHYSICAL_SCALAR_CASES_H

/* The scalar oracle scans every literal Class coordinate independently of
 * the binary-search projection. Complete closure and descending support are
 * checked on the same immutable relation before comparing physical masks. */
static XrXirStatus class_physical_scalar_receive(const EffectInvocationRead *owner,
    const EffectBodyRequest *request) {
    XrXirStatus status=effect_body_physical_match(owner->work,owner->final->physical,owner->final->material);
    if(status==XR_XIR_OK) status=effect_capture_match(owner->work,owner->final);
    for(uint32_t n=0;status==XR_XIR_OK && n<owner->final->count;++n) {
        const EffectInvocationNode *node=&owner->final->nodes[n];EffectBodyFunctionHead head={0};
        status=effect_body_request_head(request,node->body,&head);EffectInvocationBasis basis={0};
        if(status==XR_XIR_OK) status=effect_invocation_basis(owner,node->root,&basis);
        uint64_t values=(uint64_t)head.parameter_count+head.instruction_count;
        if(status==XR_XIR_OK && (values>UINT32_MAX || node->parameter_count!=head.parameter_count ||
            node->instruction_count!=head.instruction_count)) return XR_XIR_BAD_STRUCTURE;
        for(uint32_t v=0;status==XR_XIR_OK && v<=values;++v) {
            XrXirType type=head.result;
            if(v<values) status=effect_body_request_type(request,node->body,v,&type);
            if(status!=XR_XIR_OK) break;
            if(!xr_xir_type_is_class(request->domain.types,type)) continue;
            uint32_t literal=0;
            for(uint32_t p=0;p<owner->final->capture.count;++p) {
                if(!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                EffectCaptureFact fact=owner->final->capture.facts[p];
                if(fact.node!=n || fact.value!=v) continue;
                for(uint32_t bit=0;bit<3;++bit) {
                    uint32_t position=basis.parameters+bit;
                    if(fact.word==basis.fn_words+position/64 && fact.bit==position%64)
                        literal|=bit==0?EFFECT_CLASS_ROOT:bit==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
                }
            }
            uint32_t actual=0;status=effect_final_class_mask(owner->work,owner->final,n,v,&actual);
            if(status==XR_XIR_OK && (literal!=actual || !(literal&EFFECT_CLASS_KNOWN))) return XR_XIR_BAD_STRUCTURE;
            if(status==XR_XIR_OK && v<head.parameter_count && owner->final->roots[node->body]==n &&
                literal!=(EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN)) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return status;
}
#endif // XIR_CLASS_PHYSICAL_SCALAR_CASES_H
