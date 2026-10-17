/* These readers borrow actual Final and Input only. They neither create an
 * invocation flow nor reconstruct a solver's semantic inventory. */
static XrXirStatus effect_final_error_edge_matches(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t edge,bool *matched) {
    if(!final || final->material!=material || edge>=final->edge_count) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureRead read={.work=work,.final=final,
        .body=effect_body_owned_request(work,material)};
    return effect_capture_edge_complete(&read,edge,matched);
}
static XrXirStatus effect_final_error_unknown(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t node,uint32_t value,
    bool present,bool *unknown) {
    if(!final || final->material!=material) return XR_XIR_BAD_STRUCTURE;
    EffectCaptureRead read={.work=work,.final=final,
        .body=effect_body_owned_request(work,material)};
    EffectCaptureNode current={0};
    XrXirStatus status=effect_capture_node(&read,node,&current);
    if(status!=XR_XIR_OK) return status;
    bool unresolved=!present;
    for(uint32_t bit=0;bit<current.basis.parameters+3;++bit) {
        const EffectCaptureFact *fact=NULL;uint32_t origin=final->site_count+bit;
        status=effect_capture_find(&read,(EffectCaptureFact){node,value,origin/64,origin%64,0},&fact);
        if(status!=XR_XIR_OK) return status;
        if(!fact) continue;
        /* An opaque alternative with genuine targets needs an independently
         * exhaustive value proof before its Error residual can be discharged. */
        if(bit==current.basis.parameters && present) return XR_XIR_UNRESOLVED;
        unresolved=true;
    }
    *unknown=unresolved;return XR_XIR_OK;
}
