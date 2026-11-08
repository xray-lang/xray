/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_parameters.inc.c - Ordinary-context physical-use proof
 *
 * KEY CONCEPT:
 *   Closed ordinary views reuse the physical-use solver. Resolved requirement
 *   edges reach real implementation bodies; stored classifications are ignored.
 */
typedef struct EffectOrdinaryNode {
    uint32_t declaration;
    const XrXirType *arguments;
    uint32_t argument_count;
    uint32_t ordinary, owner;
    const XrXirType *physical_types, *source_parameters;
} EffectOrdinaryNode;

typedef struct EffectOrdinaryContexts {
    XrCompileResources *resources;
    EffectInputFacts *input_facts;
    EffectTerms terms;
    EffectOrdinaryNode *nodes;
    XrXirFunction *functions;
    uint32_t count, capacity, base_count, source_type_count;
    bool dense;
    XrXirDeclarations *source_declarations;
    XrXirGeneric *source_generics;
    XrXirDefaultTable source_defaults;
    XrXirEffects uses;
} EffectOrdinaryContexts;

/* Only the constructor owns these parameter-use results and its Input
 * reference. Final keeps its own retained Input; no descriptor is freed here. */
static void effect_ordinary_results_free(EffectOrdinaryContexts *contexts) {
    effect_parameters_free(&contexts->uses);
    xr_compile_resources_free(contexts->uses.outcomes_work.root);
    xr_compile_resources_free(contexts->uses.outcomes_work.root_witnesses);
    xr_compile_resources_free(contexts->uses.outcomes_work.unresolved_witnesses);
}

static bool effect_ordinary_work_empty(const EffectOrdinaryContexts *contexts) {
    const XrXirEffects *uses=&contexts->uses;
    return !uses->resources && !uses->count && !uses->outcomes_owner && !uses->root_input &&
        !uses->formula_terminals && !uses->constant_witnesses && !uses->cells &&
        !uses->refinement && !uses->invocations && !uses->result_borrow && !uses->generation &&
        effect_outcomes_empty(&uses->outcomes_work);
}

static void effect_ordinary_free(EffectOrdinaryContexts *contexts) {
    if (!contexts) return;
    xr_xir_compile_declarations_free(contexts->source_declarations);
    xr_xir_compile_generics_free(contexts->source_generics,contexts->base_count);
    effect_ordinary_results_free(contexts);
    xr_compile_resources_free(contexts->nodes);
    xr_compile_resources_free(contexts->functions);
    effect_terms_free(&contexts->terms);
    effect_input_facts_free(contexts->input_facts);
    xr_compile_resources_free(contexts);
}

/* Descriptor vectors are replaceable metadata, not permanent term payloads.
 * Each root belongs to the same finite ledger. Every admitted byte is still
 * initialized; growth publishes only after both complete vectors are copied. */






static XrXirStatus effect_ordinary_environment_same(EffectTerms *terms,
    const EffectOrdinaryNode *node,const EffectContextResolved *target,bool *equal) {
    *equal=node->declaration==target->target && node->argument_count==target->argument_count;
    XrXirTypeMatchScratch *scratch=effect_terms_type_scratch(terms);
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t a=0;*equal && a<target->argument_count && status==XR_XIR_OK;++a) {
        status=xr_xir_compile_type_substitution_matches_between_scratch(terms->remaining,
            &terms->types,&target->terms.types,NULL,0,node->arguments[a],target->arguments[a],scratch);
        if (status==XR_XIR_BAD_TYPE) { *equal=false;status=XR_XIR_OK; }
    }
    return status;
}







static XrXirStatus effect_ordinary_consumes_match(const XrXirFunction *original,const XrXirFunction *actual);
static XrXirStatus effect_ordinary_signature_match(const EffectOrdinaryContexts *contexts,
    const XrXirModule *source,uint32_t function, EffectTerms *terms);

/* Internal role products are derived from the real generating vector. No
 * incomplete public TEMPLATE, stored writer role or permission hint enters it. */
static XrXirStatus effect_body_ordinary_input_verify(const XrXirCompileContext *work,
    const XrXirModule *module,const EffectBodyOrdinaryInput *input) {
    if (!xir_compile_context_valid(work) || !module || !input || !input->contexts || !input->source)
        return XR_XIR_BAD_STRUCTURE;
    EffectOrdinaryContexts *contexts=input->contexts;const XrXirModule *source=input->source;
    if (!xir_compile_context_valid(contexts->terms.remaining) ||
        contexts->terms.remaining->resources!=work->resources || contexts->uses.resources!=work->resources ||
        contexts->terms.status!=XR_XIR_OK || !contexts->count || contexts->count>contexts->capacity ||
        !contexts->nodes || !contexts->functions || contexts->uses.count!=contexts->count ||
        !source->functions || !source->function_count || !source->types || !source->declarations ||
        !source->declarations->functions || contexts->base_count!=source->function_count ||
        contexts->base_count>contexts->count || contexts->source_type_count!=source->types->count ||
        module->functions!=contexts->functions || module->function_count!=contexts->count ||
        module->types!=&contexts->terms.types || module->provenance || !module->declarations ||
        !module->declarations->functions || !module->generics ||
        module->stage!=source->stage || module->linkage_kind!=source->linkage_kind)
        return XR_XIR_BAD_STRUCTURE;
    if (source->function_count>work->limits.functions ||
        contexts->count>effect_body_entry_limit(work)) return XR_XIR_BUDGET;

    if (source->function_count>work->limits.functions || contexts->count>effect_equation_maximum(work))
        return XR_XIR_BUDGET;
    const XrXirDeclarations *expected=source->declarations,*actual=module->declarations;
    if (actual->modules!=expected->modules || actual->module_count!=expected->module_count ||
        actual->slots!=expected->slots || actual->slot_count!=expected->slot_count ||
        actual->literals!=expected->literals || actual->literal_count!=expected->literal_count ||
        actual->root_module!=expected->root_module || actual->entry_function!=expected->entry_function ||
        actual->implementations!=expected->implementations || module->defaults!=source->defaults)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_terms_snapshot_match(&contexts->terms,source->types,contexts->source_type_count);
    if (status!=XR_XIR_OK) return status;
    for (uint32_t f=0;f<contexts->count;++f) {

        const EffectOrdinaryNode *node=&contexts->nodes[f];const XrXirFunction *function=&contexts->functions[f];
        if (node->declaration>=source->function_count || !!node->arguments!=!!node->argument_count ||
            (f<contexts->base_count && node->declaration!=f) ||
            function->parameter_count!=source->functions[node->declaration].parameter_count ||
            (function->parameter_count && (!function->parameters || !node->source_parameters ||
             (contexts->dense && !node->physical_types)))) return XR_XIR_BAD_STRUCTURE;
        status=effect_ordinary_consumes_match(&source->functions[node->declaration],function);
        if(status!=XR_XIR_OK)return status;
        XrXirFunctionIdentity identity=source->declarations->functions[node->declaration];
        if (identity.cleanup_owner) {
            if (node->owner>=contexts->count ||
                contexts->nodes[node->owner].declaration+1!=identity.cleanup_owner) return XR_XIR_BAD_STRUCTURE;
            identity.cleanup_owner=node->owner+1;
        } else if (node->owner!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        if (memcmp(&identity,&module->declarations->functions[f],sizeof(identity))) return XR_XIR_BAD_STRUCTURE;
        XrXirGeneric generic={0};
        if (f<contexts->base_count && source->generics) generic=source->generics[f];
        const XrXirGeneric *current=&module->generics[f];
        if (generic.constraints!=current->constraints || generic.parameter_count!=current->parameter_count ||
            generic.arguments!=current->arguments || generic.argument_count!=current->argument_count ||
            generic.parameter_kinds!=current->parameter_kinds) return XR_XIR_BAD_STRUCTURE;
        if (!contexts->dense) {
            status=effect_ordinary_signature_match(contexts,source,f,&contexts->terms);
            if (status!=XR_XIR_OK) return status;
        }
        const XrXirType *physical=contexts->dense?node->physical_types:node->source_parameters;
        XrXirProofContext proof={module,{XR_XIR_CONTEXT_FUNCTION,f,0}};
        for (uint32_t p=0;p<function->parameter_count;++p) {

            if (function->parameters[p]!=physical[p]) return XR_XIR_BAD_STRUCTURE;
            status=xr_xir_compile_type_use_verify(work,&proof,function->parameters[p]);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus effect_body_ordinary_identity(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *input,uint32_t function,EffectBodyOrdinaryIdentity *output) {
    if (!xir_compile_context_valid(work) || !input || !input->contexts || !input->source || !output ||
        function>=input->contexts->count || input->contexts->uses.resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;
    const EffectOrdinaryNode *node=&input->contexts->nodes[function];
    if (node->declaration>=input->source->function_count ||
        (node->owner!=UINT32_MAX && node->owner>=input->contexts->count)) return XR_XIR_BAD_STRUCTURE;

    *output=(EffectBodyOrdinaryIdentity){node->declaration,node->owner};return XR_XIR_OK;
}

/* This live ordinary owner supplies lifetime only. The complete current
 * request is compared independently before its material can be retained. */
static XrXirStatus effect_ordinary_bodies_match(const EffectOrdinaryContexts *contexts,
    const XrXirModule *source, EffectTerms *terms);

/* The retained material never aliases producer arrays. Recheck the real
 * source relation using the original ordinary matcher before retaining it. */
static XrXirStatus effect_ordinary_basis_source(const XrXirCompileContext *work,
    EffectOrdinaryContexts *basis,const XrXirModule *source) {
    if (!source || !source->types || !source->functions || !source->declarations ||
        !source->declarations->functions || !source->declarations->modules)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<basis->count;++f) {

        uint32_t declaration=basis->nodes[f].declaration;
        if (declaration>=source->function_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *actual=&source->functions[declaration];
        if ((actual->parameter_count && !actual->parameters) ||
            (actual->instruction_count && !actual->instructions) ||
            (actual->operand_count && !actual->operands) ||
            (actual->name_length && !actual->name)) return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirCompileContext *previous=basis->terms.remaining;
    basis->terms.remaining=work;
    XrXirStatus status=effect_terms_snapshot_match(&basis->terms,source->types,basis->source_type_count);
    if (status==XR_XIR_OK) status=effect_ordinary_bodies_match(basis,source,&basis->terms);
    basis->terms.remaining=previous;
    return status;
}

static XrXirStatus effect_body_ordinary_basis(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *input,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !input || !input->contexts || !input->source ||
        !input->basis || !output || *output) return XR_XIR_BAD_STRUCTURE;
    EffectOrdinaryContexts *basis=input->basis;
    if (basis==input->contexts || basis->dense || !input->contexts->dense ||
        basis->uses.resources!=work->resources || basis->terms.status!=XR_XIR_OK ||
        basis->base_count!=input->source->function_count ||
        basis->source_type_count!=input->source->types->count ||
        basis->count>basis->capacity || !basis->nodes || !basis->functions ||
        basis->uses.count!=basis->count || basis->uses.invocations ||
        !effect_body_material_shape(basis->uses.generation) ||
        basis->uses.generation->resources!=work->resources ||
        (basis->uses.generation->backbone && basis->uses.generation->backbone->backbone) ||
        !basis->uses.generation->ordinary_identities ||
        effect_body_count(basis->uses.generation)!=basis->count) return XR_XIR_BAD_STRUCTURE;

    XrXirStatus status=effect_ordinary_basis_source(work,input->basis,input->source);
    if (status!=XR_XIR_OK) return status;
    *output=basis->uses.generation;return XR_XIR_OK;
}

static XrXirStatus effect_body_ordinary_record_parent(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *input,EffectBodyMaterial **output) {
    if (!xir_compile_context_valid(work) || !input || !output || *output ||
        (input->basis && input->initial)) return XR_XIR_BAD_STRUCTURE;
    if (input->basis) return effect_body_ordinary_basis(work,input,output);
    EffectBodyMaterial *source=input->record_source?input->record_source:input->initial;
    if (!source) return XR_XIR_OK;
    if (!effect_body_material_shape(source) || source->resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;

    *output=source;return XR_XIR_OK;
}

static XrXirStatus effect_body_ordinary_record_index(const XrXirCompileContext *work,
    const EffectBodyOrdinaryInput *input,uint32_t function,uint32_t *output) {
    if (!xir_compile_context_valid(work) || !input || !input->contexts || !output ||
        function>=input->contexts->count || !input->contexts->nodes ||
        (!!input->basis==!!(input->initial || input->record_source))) return XR_XIR_BAD_STRUCTURE;

    const EffectOrdinaryNode *node=&input->contexts->nodes[function];
    *output=input->basis?node->ordinary:node->declaration;return XR_XIR_OK;
}

typedef struct EffectOrdinaryView {
    XrXirModule module;
    XrXirDeclarations declarations;
    EffectBodyOrdinaryInput input;
} EffectOrdinaryView;

/* All contextual function metadata uses contextual indices. The source
 * identity relation is verified before any rewritten cleanup owner enters it. */


#ifdef XR_XIR_EFFECT_CONTEXT_TESTS
static XrXirStatus effect_ordinary_seed_rebuild(const XrXirCompileContext *context,
    const XrXirModule *source, const EffectContextResolved *seed, EffectContextResolved **output) {
    EffectContextRequest request={source,&seed->terms.types,seed->origin_arguments,
        seed->origin_argument_count,seed->function,seed->instruction};
    EffectContextResolved *fresh=NULL;
    XrXirStatus status=effect_context_resolve(context,&request,&fresh);
    if (status!=XR_XIR_OK) return status;
    if (!fresh->known || fresh->target!=seed->target || fresh->family!=seed->family ||
        fresh->argument_count!=seed->argument_count) status=XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    for (uint32_t a=0;a<seed->argument_count && status==XR_XIR_OK;++a) {
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &seed->terms.types,&fresh->terms.types,NULL,0,seed->arguments[a],fresh->arguments[a],&scratch);
        if (status==XR_XIR_BAD_TYPE) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) { effect_context_free(fresh);return status; }
    *output=fresh;return XR_XIR_OK;
}

/* Original generic definitions remain in the view. Only authentic closed
 * ordinary environments add private entries; callable bounds are still whole
 * declared bounds until physical use is independently rederived. */
static XrXirStatus effect_ordinary_classify(const XrXirCompileContext *context,
    const XrXirModule *source, const EffectContextResolved *seed,
    EffectInputFacts *input_facts, EffectOrdinaryContexts **output, uint32_t *selected) {
    if (!xir_compile_context_valid(context) || !source || !source->types ||
        !source->functions || !source->function_count || !seed || !seed->known ||
        seed->resources!=context->resources || !output || *output || !selected)
        return XR_XIR_BAD_STRUCTURE;
    if (source->function_count>context->limits.functions) return XR_XIR_BUDGET;
    EffectContextResolved *fresh=NULL;
    XrXirStatus status=effect_ordinary_seed_rebuild(context,source,seed,&fresh);
    if (status!=XR_XIR_OK) return status;
    EffectOrdinaryContexts *contexts=xir_compile_calloc(context,1,sizeof(*contexts),&status);
    if (!contexts) { effect_context_free(fresh);return status; }
    contexts->resources=context->resources;
    status=effect_input_facts_retain(context,input_facts);
    if(status!=XR_XIR_OK){effect_context_free(fresh);effect_ordinary_free(contexts);return status;}
    contexts->input_facts=input_facts;
    contexts->terms.remaining=context;contexts->base_count=source->function_count;
    contexts->source_type_count=source->types->count;
    status=effect_terms_seed(&contexts->terms,source->types);
    if (status==XR_XIR_OK) status=effect_ordinary_snapshot(contexts,source);
    if (status==XR_XIR_OK) status=effect_ordinary_capacity(contexts,source->function_count);
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        contexts->nodes[f]=(EffectOrdinaryNode){.declaration=f,.ordinary=f,.owner=UINT32_MAX};contexts->count=f+1;
        status=effect_ordinary_function(contexts,source,f);
    }
    uint32_t target=UINT32_MAX;
    if (status==XR_XIR_OK) status=effect_ordinary_intern(contexts,source,fresh,fresh->function,&target);
    effect_context_free(fresh);
    for (uint32_t f=0;f<contexts->count && status==XR_XIR_OK;++f)
        status=effect_ordinary_expand(contexts,source,f);
    EffectGraph graph={0};EffectOrdinaryView view={0};
    contexts->uses.resources=context->resources;contexts->uses.count=contexts->count;contexts->uses.input_facts=contexts->input_facts;
    if (status==XR_XIR_OK) status=effect_ordinary_view(contexts,source,&view);
    if (status==XR_XIR_OK) {
        graph.ordinary=view.input;
        status=effect_parameters_derive(&view.module,&contexts->uses,&graph,context);
    }
    effect_graph_free(&graph);
    if (status!=XR_XIR_OK) { effect_ordinary_free(contexts);return status; }
    contexts->terms.remaining=NULL;
    *selected=target;*output=contexts;return XR_XIR_OK;
}

#endif
