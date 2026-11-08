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

/* Descriptor vectors are replaceable metadata, not permanent term payloads.
 * Each root belongs to the same finite ledger. Every admitted byte is still
 * initialized; growth publishes only after both complete vectors are copied. */

static XrXirStatus effect_ordinary_consumes_match(const XrXirFunction *original,const XrXirFunction *actual);
static XrXirStatus effect_ordinary_signature_match(const EffectOrdinaryContexts *contexts,
    const XrXirModule *source,uint32_t function, EffectTerms *terms);

/* Internal role products are derived from the real generating vector. No
 * incomplete public TEMPLATE, stored writer role or permission hint enters it. */

/* This live ordinary owner supplies lifetime only. The complete current
 * request is compared independently before its material can be retained. */
static XrXirStatus effect_ordinary_bodies_match(const EffectOrdinaryContexts *contexts,
    const XrXirModule *source, EffectTerms *terms);

/* The retained material never aliases producer arrays. Recheck the real
 * source relation using the original ordinary matcher before retaining it. */

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
