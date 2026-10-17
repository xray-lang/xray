/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_final_flow.inc.c - Current finite returned-source projection
 *
 * KEY CONCEPT:
 *   A receiving flow consumes the sole Final relation. Constructor discovery
 *   records have already died and cannot supply a second source inventory.
 */
static XrXirStatus effect_invocation_final_return_join(EffectInvocationFlow *flow,
    uint32_t node,uint32_t returned,uint32_t destination) {
    const EffectInvocationRead *owner=flow->read;const XrXirCompileContext *work=owner->work;
    if(owner->discovery || owner->roles.construction || flow->owner) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *final=owner->final;
    XrXirStatus status=effect_role_range(work,final,1,sizeof(*final));if(status!=XR_XIR_OK) return status;
    if(node>=final->count || flow->node>=final->count || final->count>final->capacity ||
        final->capture.count>final->capture.capacity || !!final->capture.facts!=!!final->capture.capacity)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_material_frame(work,final->material);if(status!=XR_XIR_OK) return status;
    if(final->material!=flow->body.owner) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->material->functions,
        effect_body_count(final->material),sizeof(*final->material->functions));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->capture.facts,final->capture.capacity,sizeof(*final->capture.facts));
    if(status!=XR_XIR_OK) return status;
    EffectCaptureRead read={.work=work,.final=final,.body=effect_body_owned_request(work,final->material)};
    EffectCaptureNode target={0};status=effect_capture_node(&read,node,&target);if(status!=XR_XIR_OK) return status;
    if(!xir_compile_work(work,8)) return XR_XIR_BUDGET;
    if(target.values!=returned || final->nodes[node].root!=final->nodes[flow->node].root ||
        target.basis.parameters!=flow->basis.parameters || target.basis.origin_words!=flow->basis.origin_words ||
        target.basis.fn_words!=flow->basis.fn_words || target.basis.cell_words!=flow->basis.cell_words ||
        target.basis.row_words!=flow->basis.row_words) return XR_XIR_BAD_STRUCTURE;
    uint64_t *row=effect_invocation_row(flow,destination);if(!row) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureFact key={node,returned,0,0,0};uint32_t at=0;
    status=effect_capture_lower(&read,&key,&at);if(status!=XR_XIR_OK) return status;
    bool genuine=false;
    for(;at<final->capture.count;++at) {
        if(!xir_compile_work(work,6)) return XR_XIR_BUDGET;
        const EffectCaptureFact *fact=&final->capture.facts[at];
        if(fact->node!=node || fact->value!=returned || fact->word>=target.basis.fn_words) break;
        if(fact->bit>=64) return XR_XIR_BAD_STRUCTURE;
        if(fact->word<target.basis.origin_words) {
            if((uint64_t)fact->word*64+fact->bit>=final->site_count) continue;
            genuine=true;
        } else if(!genuine) continue;
        uint64_t joined=row[fact->word]|(UINT64_C(1)<<fact->bit);
        if(joined!=row[fact->word]) {
            if(!xir_compile_work(work,sizeof(*row))) return XR_XIR_BUDGET;
            row[fact->word]=joined;flow->changed=true;
        }
    }
    return XR_XIR_OK;
}

/* Class flags occupy the same finite cells as callable and Cell captures.
 * Actual type and current geometry select them; no discovery rank is copied. */
static XrXirStatus effect_final_class_mask(const XrXirCompileContext *work,
    const EffectInvocationFinal *final,uint32_t index,uint32_t value,uint32_t *output) {
    if(!output || *output || !final) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_range(work,final,1,sizeof(*final));if(status!=XR_XIR_OK) return status;
    if(index>=final->count || final->count>final->capacity || final->capture.count>final->capture.capacity ||
        !!final->capture.facts!=!!final->capture.capacity) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_material_frame(work,final->material);
    if(status==XR_XIR_OK) status=effect_role_range(work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->material->functions,
        effect_body_count(final->material),sizeof(*final->material->functions));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->capture.facts,final->capture.capacity,sizeof(*final->capture.facts));
    if(status!=XR_XIR_OK) return status;
    EffectCaptureRead read={.work=work,.final=final,.body=effect_body_owned_request(work,final->material)};
    EffectCaptureNode node={0};status=effect_capture_node(&read,index,&node);
    uint32_t kind=0;
    if(status==XR_XIR_OK) status=effect_capture_kind(&read,&node,value,&kind,NULL);
    if(status!=XR_XIR_OK) return status;
    if(kind!=EFFECT_CAPTURE_CLASS || value>node.values) return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=0;
    for(uint32_t flag=0;flag<3;++flag) {
        uint32_t bit=node.basis.parameters+flag;const EffectCaptureFact *fact=NULL;
        EffectCaptureFact key={index,value,node.basis.fn_words+bit/64,bit%64,0};
        status=effect_capture_find(&read,key,&fact);if(status!=XR_XIR_OK) return status;
        if(!fact) continue;
        if(fact->rank>=final->capture.count) return XR_XIR_BAD_STRUCTURE;
        mask|=flag==0?EFFECT_CLASS_ROOT:flag==1?EFFECT_CLASS_UNKNOWN:EFFECT_CLASS_KNOWN;
    }
    if(!mask) return XR_XIR_UNRESOLVED;
    if(!(mask&EFFECT_CLASS_KNOWN)) return XR_XIR_BAD_STRUCTURE;
    *output=mask;return XR_XIR_OK;
}
