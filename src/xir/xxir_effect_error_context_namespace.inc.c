static XrXirStatus effect_final_inventory_sites(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final);
static XrXirStatus effect_final_error_edge_matches(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t edge,bool *matched);
static XrXirStatus effect_final_error_unknown(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t node,uint32_t value,
    bool present,bool *unknown);

/* A factory's returned carrier has a separate Error equation from its escaping
 * errors. The solver and receiving proof use this same current node census. */
static XrXirStatus error_finite_returned_node(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectInvocationFinal *final,uint32_t node,bool *output) {
    if(!output || node>=final->count || final->nodes[node].body>=effect_body_count(material))
        return XR_XIR_BAD_STRUCTURE;
    *output=false;
    uint32_t function=final->nodes[node].body;
    const XrXirDeclarations *declarations=material->domain.declarations;
    if(!xir_compile_work(work,3)) return XR_XIR_BUDGET;
    if(declarations && declarations->functions &&
        (declarations->functions[function].promises&XR_XIR_FUNCTION_GENERATOR)) return XR_XIR_OK;
    EffectBodyRequest body=effect_body_owned_request(work,material);
    EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(&body,function,&head);
    if(status!=XR_XIR_OK) return status;
    const XrXirTypeNode *type=xr_xir_type_node(material->domain.types,head.result);
    *output=type && (type->kind==XR_XIR_TYPE_TASK || type->kind==XR_XIR_TYPE_ITERATOR);
    return XR_XIR_OK;
}
/* Declaration roots lead, followed by other invocation nodes, then returned
 * carrier rows in node order. Row purpose is part of identity, not a new call. */
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
                    *output=final->count;*contextual=true;
                    for(uint32_t n=0;n<final->count;++n) {
                        bool returned=false;status=error_finite_returned_node(work,proof->material,final,n,&returned);
                        if(status!=XR_XIR_OK) return status;
                        if(returned) {
                            if(*output==UINT32_MAX) return XR_XIR_BUDGET;
                            ++*output;
                        }
                    }
                    return XR_XIR_OK;
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
    uint32_t normal=proof->contextual?final->count:proof->functions;
    if(context>=normal) {
        uint32_t at=normal;
        for(uint32_t n=0;n<final->count;++n) {
            bool returned=false;
            XrXirStatus status=error_finite_returned_node(work,proof->material,final,n,&returned);
            if(status!=XR_XIR_OK) return status;
            if(returned && at++==context) { *output=n;return XR_XIR_OK; }
        }
        return XR_XIR_BAD_STRUCTURE;
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
    const EffectErrorFinite *proof,uint32_t node,bool returned,uint32_t *output) {
    if(node>=proof->invocations->count) return XR_XIR_BAD_STRUCTURE;
    if(!proof->contextual) {
        if(returned) return XR_XIR_BAD_STRUCTURE;
        *output=proof->invocations->nodes[node].body;
        return *output<proof->functions?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    for(uint32_t c=0;c<proof->contexts;++c) {
        if(!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if(proof->rows[c].returned==(uint32_t)returned && proof->rows[c].node==node) {
            *output=c;return XR_XIR_OK;
        }
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

/* A callback-free walk owns one freshly framed stack identity. Rows and node
 * identities are still read from the actual proof; no semantic inventory is
 * copied. A construction walk ends before a buffer can grow. */
static XrXirStatus error_finite_context_read(const EffectErrorFiniteRead *read,
    uint32_t context,uint32_t *output) {
    if(!read || !read->proof || !read->work || !output) return XR_XIR_BAD_STRUCTURE;
    if(!read->current) return error_finite_context_body(read->work,read->proof,context,output);
    const EffectErrorFiniteView *current=read->current;const EffectErrorFinite *proof=read->proof;
    if(current->work!=read->work || current->proof!=proof || current->material!=proof->material ||
        proof->resources!=read->work->resources || context>=proof->contexts) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *final=proof->invocations;uint32_t node=proof->rows[context].node;
    if(final->material!=current->material || node>=final->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t function=final->nodes[node].body;
    if(function>=proof->functions) return XR_XIR_BAD_STRUCTURE;
    if(!xir_compile_work(read->work,7+sizeof(*output))) return XR_XIR_BUDGET;
    *output=function;return XR_XIR_OK;
}
