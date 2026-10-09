/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_query.inc.c - Structural selection from authentic contexts
 */
static XrXirStatus effect_context_owner_project(const XrXirCompileContext *context,
    const XrXirModule *source, XrXirEffects *effects, const EffectContextOwner *fresh) {
    if (!xir_compile_work(context,(uint64_t)effects->count*
        (sizeof(XrXirRootEffects)+2*sizeof(XrXirRootEffectWitness)))) {
        return XR_XIR_BUDGET;
    }
    for (uint32_t f=0;f<effects->count;++f) {
        if (source->generics && source->generics[f].parameter_count) continue;
        effects->root[f]=fresh->forest->facts[f];
        effects->root_witnesses[f]=fresh->forest->witnesses[f];
        effects->unresolved_witnesses[f]=fresh->forest->witnesses[fresh->forest->count+f];
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_context_owner_refresh(const XrXirCompileContext *context,
    const XrXirModule *source, XrXirEffects *effects) {
    if (!source->provenance || source->provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE ||
        !source->declarations || !source->declarations->functions) return XR_XIR_OK;
    EffectContextOwner *fresh=NULL;
    XrXirStatus status=effect_context_owner_build(context,source,effects->refinement,&fresh);
    if (status!=XR_XIR_OK) return status;
    status=effect_context_owner_project(context,source,effects,fresh);
    if (status!=XR_XIR_OK) { effect_context_owner_free(fresh);return status; }
    EffectContextOwner *old=effects->contexts;effects->contexts=fresh;
    effect_context_owner_free(old);return XR_XIR_OK;
}

XR_FUNC bool xir_effects_context_available(const XrXirCompileContext *context,
    const XrXirEffects *effects) {
    return xir_compile_context_valid(context) && effects && effects->resources==context->resources &&
        effects->contexts!=NULL;
}

XR_FUNC XrXirStatus xir_effects_context_refresh(const XrXirCompileContext *context,
    const XrXirModule *source, const XrXirEffects *effects) {
    if (!xir_effects_context_matches(context,effects,source ? source->function_count : 0) ||
        !source) return XR_XIR_BAD_STRUCTURE;
    XrXirModule normalized=*source;
    if (!normalized.types) normalized.types=&effect_empty_types;
    source=&normalized;
    if (!effects->contexts) return XR_XIR_OK;
    if (effects->refinement) {
        XrXirStatus bound_status=effect_refinement_match(context,source,effects->refinement);
        if (bound_status!=XR_XIR_OK) return bound_status;
    }
    EffectOrdinaryContexts *ordinary=effects->contexts->ordinary;
    ordinary->terms.remaining=context;
    XrXirStatus status=effect_terms_snapshot_match(&ordinary->terms,source->types,ordinary->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(ordinary,source);
    ordinary->terms.remaining=NULL;
    if (status==XR_XIR_BAD_TYPE || status==XR_XIR_BAD_STRUCTURE)
        return effect_context_owner_refresh(context,source,(XrXirEffects *)effects);
    return status;
}

static XrXirStatus effect_context_origin_match(const XrXirCompileContext *context,
    const EffectOrdinaryContexts *dense, const XirEffectContextInput *input,
    uint32_t index, bool *same) {
    const EffectOrdinaryNode *node=&dense->nodes[index];
    const XrXirOrigin *origin=input->origin;
    *same=node->declaration==origin->function && node->argument_count==origin->argument_count;
    if (!*same) return XR_XIR_OK;
    uint32_t count=dense->functions[index].parameter_count;
    if (!origin->effect_argument_count && count) {
        *same=index<dense->base_count && !origin->argument_count;return XR_XIR_OK;
    }
    /* Zero physical parameters form a complete empty vector. Its ordinary
     * generic identity must still match exactly; it is not the base identity.
     * A missing nonempty physical vector retains the base-only rule above. */
    if (origin->effect_argument_count!=count || (count && !origin->effect_arguments)) {
        *same=false;return XR_XIR_OK;
    }
    XrXirTypeMatchScratch scratch={context->resources,NULL};XrXirStatus status=XR_XIR_OK;
    for (uint32_t a=0;a<node->argument_count && *same && status==XR_XIR_OK;++a) {
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &dense->terms.types,input->types,NULL,0,node->arguments[a],origin->arguments[a],&scratch);
        if (status==XR_XIR_BAD_TYPE) { *same=false;status=XR_XIR_OK; }
    }
    for (uint32_t p=0;p<count && *same && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(context,1)) { status=XR_XIR_BUDGET;break; }
        if (origin->effect_arguments[p].parameter!=p) { *same=false;break; }
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &dense->terms.types,input->types,NULL,0,node->physical_types[p],origin->effect_arguments[p].type,&scratch);
        if (status==XR_XIR_BAD_TYPE) { *same=false;status=XR_XIR_OK; }
    }
    xr_xir_type_match_scratch_free(&scratch);return status;
}

static XrXirStatus effect_context_owners_match(const XrXirCompileContext *context,
    const EffectOrdinaryContexts *dense, const XirEffectContextInput *input,
    uint32_t selected, bool *same) {
    uint32_t owner=dense->nodes[selected].owner;
    for (uint32_t a=0;a<input->owner_count && *same;++a) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (owner>=dense->count) { *same=false;break; }
        XirEffectContextInput parent=*input;parent.origin=&input->owners[a];
        XrXirStatus status=effect_context_origin_match(context,dense,&parent,owner,same);
        if (status!=XR_XIR_OK) return status;
        owner=dense->nodes[owner].owner;
    }
    /* Every selected context consumes its complete lexical owner chain,
     * including zero-arity generic contexts with an empty physical vector. */
    if (*same && owner!=UINT32_MAX) *same=false;
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_context_select(const XrXirCompileContext *context,
    const XrXirEffects *effects, const XirEffectContextInput *input, XirEffectContextView *output) {
    if (!xir_compile_context_valid(context) || !effects || effects->resources!=context->resources ||
        !effects->contexts || !input || !input->source || !input->origin || !output ||
        input->origin->function>=input->source->function_count ||
        !!input->origin->arguments!=!!input->origin->argument_count ||
        !!input->owners!=!!input->owner_count || input->owner_count>effects->count) return XR_XIR_BAD_STRUCTURE;
    XrXirModule normalized=*input->source;XirEffectContextInput request=*input;
    if (!normalized.types) normalized.types=&effect_empty_types;
    /* A null actual pool is the canonical empty pool. It cannot supply any
     * constructed physical argument or erase the retained nominal domain. */
    if (!request.types) request.types=&effect_empty_types;
    request.source=&normalized;input=&request;
    const EffectOrdinaryContexts *dense=effects->contexts->dense;
    EffectOrdinaryContexts *ordinary=effects->contexts->ordinary;
    ordinary->terms.remaining=context;
    XrXirStatus status=effect_terms_snapshot_match(&ordinary->terms,input->source->types,dense->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(ordinary,input->source);
    if (status==XR_XIR_OK) status=input->types->interfaces ?
        effect_terms_domain_match(&ordinary->terms,input->types) :
        effect_terms_projected_domain(&ordinary->terms,input->types);
    ordinary->terms.remaining=NULL;
    if (status!=XR_XIR_OK) return status;
    uint32_t caller=UINT32_MAX;
    for (uint32_t f=0;f<dense->count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        bool same=false;status=effect_context_origin_match(context,dense,input,f,&same);
        if (status==XR_XIR_OK && same) status=effect_context_owners_match(context,dense,input,f,&same);
        if (status!=XR_XIR_OK) return status;
        if (same) {
            if (caller!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            caller=f;
        }
    }
    if (caller==UINT32_MAX) return XR_XIR_BAD_TYPE;
    uint32_t target=caller;
    if (input->instruction!=UINT32_MAX) {
        const XrXirFunction *original=&input->source->functions[input->origin->function];
        if (input->instruction>=original->instruction_count ||
            !effect_context_family(original->instructions[input->instruction].op)) return XR_XIR_BAD_STRUCTURE;
        const XrXirInstruction *op=&dense->functions[caller].instructions[input->instruction];
        if (op->immediate<0 || (uint64_t)op->immediate>=dense->count) return XR_XIR_BAD_STRUCTURE;
        target=(uint32_t)op->immediate;
        if (caller<dense->base_count && input->source->generics &&
            input->source->generics[input->origin->function].parameter_count) return XR_XIR_BAD_TYPE;
    }
    if (!xir_compile_work(context,sizeof(*output))) return XR_XIR_BUDGET;
    const XrXirRootEffects *facts=&effects->contexts->forest->facts[target];
    uint32_t edge_mask=(facts->requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (facts->unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    if (input->instruction!=UINT32_MAX) {
        status=effect_formula_edge_mask(context,&dense->terms.types,&dense->uses,dense->functions,caller,
            &dense->functions[caller].instructions[input->instruction],target,&edge_mask);
        if (status!=XR_XIR_OK) return status;
    }
    *output=(XirEffectContextView){&dense->terms.types,&dense->functions[target],
        dense->nodes[target].physical_types,dense->nodes[target].arguments,dense->nodes[target].declaration,
        dense->functions[target].parameter_count,dense->nodes[target].argument_count,
        !!(edge_mask&XR_XIR_CALLABLE_ROOT_REQUIRED),!!(edge_mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)};
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_reference_root(const XrXirCompileContext *context,
    const XrXirEffects *effects, const XrXirModule *module,
    uint32_t function, uint32_t instruction, uint32_t *output) {
    if (!output || !module || !xir_effects_context_matches(context,effects,module->function_count) ||
        function>=module->function_count || instruction>=module->functions[function].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&module->functions[function].instructions[instruction];
    if (op->op!=XR_XIR_FUNCTION_REF || op->immediate<0 || (uint64_t)op->immediate>=effects->count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t mask=0;
    if (effects->contexts && (!module->generics || !module->generics[function].parameter_count)) {
        XrXirOrigin origin={.function=function};
        XirEffectContextInput input={module,module->types,&origin,instruction,NULL,0};
        XirEffectContextView selected={0};
        XrXirStatus status=xir_effects_context_select(context,effects,&input,&selected);
        if (status!=XR_XIR_OK) return status;
        mask=(selected.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (selected.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    } else if (effects->contracts && !effects->contexts) {
        XrXirStatus status=effect_formula_edge_mask(context,module->types,effects,module->functions,
            function,op,(uint32_t)op->immediate,&mask);
        if (status!=XR_XIR_OK) return status;
    } else {
        const XrXirRootEffects *facts=&effects->root[op->immediate];
        mask=(facts->requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (facts->unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    }
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    *output=mask;return XR_XIR_OK;
}

static XrXirStatus effect_context_owner_trace(const XrXirCompileContext *context,
    const XrXirEffects *effects, uint32_t function, XrXirRootCauseTrace **output) {
    if (!xir_effects_context_matches(context,effects,effects ? effects->count : 0) ||
        function>=effects->count || !effects->contexts || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    return effect_context_forest_trace(context,effects->contexts->forest,function,output);
}
