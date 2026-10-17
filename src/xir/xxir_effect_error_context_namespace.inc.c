static XrXirStatus effect_final_inventory_sites(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final);
static XrXirStatus effect_final_error_edge_matches(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t edge,bool *matched);
static XrXirStatus effect_final_error_unknown(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t node,uint32_t value,
    bool present,bool *unknown);

/* Error rows keep the declaration prefix, then every remaining real invocation
 * node in node order. This mapping stores no second callable environment. */
static XrXirStatus error_finite_context_count(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,uint32_t *output,bool *contextual) {
    const EffectInvocationFinal *final=proof->invocations;
    XrXirStatus status=effect_role_range(work,final,1,sizeof(*final));
    if(status!=XR_XIR_OK) return status;
    if(final->material!=proof->material || !final->count || final->count>final->capacity)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,proof->material->functions,proof->functions,sizeof(*proof->material->functions));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status==XR_XIR_OK) status=effect_role_range(work,final->roots,proof->functions,sizeof(*final->roots));
    if(status!=XR_XIR_OK) return status;
    for(uint32_t f=0;f<proof->functions;++f) {
        if(!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        uint32_t node=final->roots[f];
        if(node>=final->count || final->nodes[node].body!=f) return XR_XIR_BAD_STRUCTURE;
    }
    *output=proof->functions;*contextual=false;
    if(!final->site_count) return XR_XIR_OK;
    status=effect_final_inventory_sites(work,proof->material,final);
    if(status!=XR_XIR_OK) return status;
    EffectBodyRead body=effect_body_compile_read(work);
    for(uint32_t f=0;f<proof->functions;++f) {
        const EffectBodyFunction *head=&proof->material->functions[f];
        EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
        for(uint32_t run=0;run<head->run_count;++run) {
            const EffectBodyRun *ref=NULL;status=effect_body_range_ref(&body,head,&cursor,&ref);
            if(status!=XR_XIR_OK) return status;
            uint32_t count=(ref->mask&1)?ref->count:1;
            for(uint32_t i=0;i<count;++i) {
                uint32_t opcode=XR_XIR_INVALID;
                status=effect_body_run_word(&body,ref,ref->first+i,0,&opcode);
                if(status!=XR_XIR_OK) return status;
                if(opcode==XR_XIR_CALL_INDIRECT || opcode==XR_XIR_INVOKE_INDIRECT) {
                    *output=final->count;*contextual=true;return XR_XIR_OK;
                }
            }
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus error_finite_context_node(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,uint32_t context,uint32_t *output) {
    if(context>=proof->contexts) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *final=proof->invocations;
    if(context<proof->functions) {
        if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        uint32_t node=final->roots[context];
        if(node>=final->count || final->nodes[node].body!=context) return XR_XIR_BAD_STRUCTURE;
        *output=node;return XR_XIR_OK;
    }
    uint32_t at=proof->functions;
    for(uint32_t n=0;n<final->count;++n) {
        bool root=false;
        for(uint32_t f=0;f<proof->functions;++f) {
            if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if(final->roots[f]==n) root=true;
        }
        if(root) continue;
        if(at++==context) { *output=n;return XR_XIR_OK; }
    }
    return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus error_finite_node_context(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,uint32_t node,uint32_t *output) {
    if(node>=proof->invocations->count) return XR_XIR_BAD_STRUCTURE;
    if(!proof->contextual) {
        *output=proof->invocations->nodes[node].body;
        return *output<proof->functions?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t c=0;c<proof->contexts;++c) {
        if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if(proof->rows[c].node==node) { *output=c;return XR_XIR_OK; }
    }
    return XR_XIR_BAD_STRUCTURE;
}
/* Allocation callbacks may overwrite a proposed row. Each actual body read
 * frames its present pointers and ordinal before touching either payload. */
static XrXirStatus error_finite_context_body(const XrXirCompileContext *work,
    const EffectErrorFinite *proof,uint32_t context,uint32_t *output) {
    XrXirStatus status=effect_role_range(work,proof,1,sizeof(*proof));
    if(status!=XR_XIR_OK) return status;
    if(!output || proof->resources!=work->resources || context>=proof->contexts)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_material_frame(work,proof->material);
    if(status==XR_XIR_OK) status=effect_role_range(work,proof->invocations,1,sizeof(*proof->invocations));
    if(status==XR_XIR_OK) status=effect_role_range(work,proof->rows,proof->contexts,sizeof(*proof->rows));
    if(status!=XR_XIR_OK) return status;
    const EffectInvocationFinal *final=proof->invocations;
    if(!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    uint32_t node=proof->rows[context].node;
    if(final->material!=proof->material || final->count>final->capacity || node>=final->count)
        return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,final->nodes,final->capacity,sizeof(*final->nodes));
    if(status!=XR_XIR_OK) return status;
    uint32_t function=final->nodes[node].body;
    if(function>=effect_body_count(proof->material)) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,proof->material->functions,effect_body_count(proof->material),
        sizeof(*proof->material->functions));
    if(status==XR_XIR_OK) *output=function;
    return status;
}
