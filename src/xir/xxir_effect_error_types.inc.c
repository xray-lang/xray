/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_error_types.inc.c - Same-owner substituted Error type storage
 *
 * The original Input remains exact. Only the finite Error certificate owns
 * the extra structural identities produced by actual generic substitutions.
 */
static XrXirStatus error_finite_types_frame(const XrXirCompileContext *work,const EffectErrorFinite *proof) {
    const XrXirTypes *input=proof->material->domain.types,*types=error_finite_types(proof);
    if(types==input) return XR_XIR_OK;
    if(!xir_compile_work(work,6)) return XR_XIR_BUDGET;
    if(types->count<input->count || types->count>XR_XIR_CONSTRUCTED_TYPE_LIMIT-XR_XIR_CONSTRUCTED_TYPE_BASE ||
        types->nominals!=input->nominals || types->interfaces!=input->interfaces) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *domains[2]={input,types};
    for(uint32_t d=0;d<2;++d) {
        const XrXirTypes *domain=domains[d];
        XrXirStatus status=effect_role_range(work,domain->nodes,domain->count,sizeof(*domain->nodes));
        if(status!=XR_XIR_OK) return status;
        for(uint32_t t=0;t<domain->count;++t) {
            const XrXirTypeNode *node=&domain->nodes[t];
            status=effect_role_range(work,node->parameters,node->parameter_count,sizeof(*node->parameters));
            if(status==XR_XIR_OK) status=effect_role_range(work,node->nominal.arguments,
                node->nominal.argument_count,sizeof(*node->nominal.arguments));
            if(status==XR_XIR_OK) status=effect_role_range(work,node->nominal.fields,
                node->nominal.field_count,sizeof(*node->nominal.fields));
            if(status!=XR_XIR_OK) return status;
        }
    }
    XrXirTypes prefix=*types;prefix.count=input->count;
    return effect_body_type_nodes_compare(work,&prefix,input,NULL);
}
