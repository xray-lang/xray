/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_forest.inc.c - Owned contextual cause certificates
 *
 * KEY CONCEPT:
 *   Public ordinals identify declarations. Private context ordinals choose the
 *   exact environment and forest, and survive release of every input owner.
 */
typedef struct EffectForestNode {
    uint32_t declaration, argument_count, parameter_count, constant_mask, owner;
    const XrXirType *arguments, *physical_types;
    const uint8_t *parameter_kinds;
} EffectForestNode;

typedef struct EffectContextCertificate {
    XrXirRootEffectWitness witness;
    uint32_t next, parameter_owner;
    uint8_t domain, next_domain;
    EffectForestNode subject, target;
} EffectContextCertificate;

typedef struct EffectContextForest {
    /* Type storage belongs to the transferred immutable invocation owner.
     * This arena owns only forest metadata and construction scratch. */
    EffectTerms terms;
    XrCompileResources *resources;
    uint32_t count;
    EffectForestNode *nodes;
    XrXirRootEffects *facts;
    XrXirRootEffectWitness *witnesses;
    EffectContextCertificate *certificates;
    EffectInvocationCertificate *invocations;
    uint8_t **requirements;
} EffectContextForest;

static void effect_context_forest_free(EffectContextForest *forest) {
    if (!forest) return;
    /* Release all borrowing metadata before its retained type owner. */
    effect_terms_free(&forest->terms);
    effect_invocation_certificate_free(forest->invocations);
    xr_compile_resources_free(forest);
}

static bool effect_context_witness_same(const XrXirRootEffectWitness *a,
    const XrXirRootEffectWitness *b) {
    return a->cause==b->cause && a->instruction==b->instruction && a->callee==b->callee &&
        a->slot==b->slot && a->distance==b->distance;
}

static XrXirStatus effect_context_forest_node(EffectContextForest *forest,
    const EffectOrdinaryContexts *contexts, uint32_t index) {
    if (!xir_compile_work(forest->terms.remaining,
        sizeof(EffectOrdinaryNode)+sizeof(EffectForestNode)+sizeof(XrXirRootEffects))) return XR_XIR_BUDGET;
    EffectOrdinaryNode origin=contexts->nodes[index];
    const XrXirFunction *function=&contexts->functions[index];
    const XrXirFunctionEffectContract *contract=&contexts->uses.contracts[index];
    XrXirType *arguments=origin.argument_count ?
        effect_terms_alloc(&forest->terms,origin.argument_count,sizeof(*arguments)) : NULL;
    XrXirType *physical=function->parameter_count ?
        effect_terms_alloc(&forest->terms,function->parameter_count,sizeof(*physical)) : NULL;
    uint8_t *kinds=function->parameter_count ?
        effect_terms_alloc(&forest->terms,function->parameter_count,sizeof(*kinds)) : NULL;
    if ((origin.argument_count && !arguments) || (function->parameter_count && (!physical || !kinds)))
        return forest->terms.status;
    for (uint32_t a=0;a<origin.argument_count;++a) {
        if (!xir_compile_work(forest->terms.remaining,sizeof(*arguments)+1)) return XR_XIR_BUDGET;
        arguments[a]=origin.arguments[a];
    }
    for (uint32_t p=0;p<function->parameter_count;++p) {
        if (!xir_compile_work(forest->terms.remaining,sizeof(*physical)+sizeof(*kinds)+1)) return XR_XIR_BUDGET;
        physical[p]=function->parameters[p];kinds[p]=(uint8_t)contract->parameters[p].kind;
    }
    forest->nodes[index]=(EffectForestNode){.declaration=origin.declaration,.argument_count=origin.argument_count,
        .parameter_count=function->parameter_count,.constant_mask=contract->formula.constant_mask,
        .owner=origin.owner,.arguments=arguments,.physical_types=physical,.parameter_kinds=kinds};
    forest->facts[index]=contexts->uses.root[index];
    return XR_XIR_OK;
}

static XrXirStatus effect_context_forest_requirements(const XrXirCompileContext *work,
    const XrXirModule *source,const EffectOrdinaryContexts *contexts,EffectContextForest *forest) {
    forest->requirements=effect_terms_alloc(&forest->terms,forest->count,sizeof(*forest->requirements));
    if (!forest->requirements) return forest->terms.status;
    for (uint32_t f=0;f<forest->count;++f) {
        uint32_t declaration=contexts->nodes[f].declaration;
        if (declaration>=source->function_count) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *original=&source->functions[declaration];
        const XrXirFunction *actual=&contexts->functions[f];bool any=false;
        if (original->instruction_count!=actual->instruction_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t i=0;i<original->instruction_count;++i) {
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            if (original->instructions[i].op==XR_XIR_CALL_REQUIREMENT) any=true;
        }
        uint8_t *bits=any?effect_terms_alloc(&forest->terms,original->instruction_count,1):NULL;
        if (any && !bits) return forest->terms.status;
        if (any) for (uint32_t i=0;i<original->instruction_count;++i) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            bits[i]=(uint8_t)(original->instructions[i].op==XR_XIR_CALL_REQUIREMENT);
            if (bits[i] && actual->instructions[i].op!=XR_XIR_CALL_REQUIREMENT &&
                actual->instructions[i].op!=XR_XIR_CALL) return XR_XIR_BAD_STRUCTURE;
        }
        forest->requirements[f]=bits;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_forest_types(const XrXirCompileContext *work,
    const EffectOrdinaryContexts *contexts,EffectTerms *output);
static XrXirStatus effect_invocation_forest_records(const XrXirCompileContext *work,
    EffectContextForest *forest,const EffectOrdinaryContexts *contexts);
static XrXirStatus effect_invocation_forest_trace(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t function,XrXirRootCauseTrace **output);
static XrXirStatus effect_invocation_context_edge_mask(const XrXirCompileContext *work,
    const EffectContextForest *forest,uint32_t caller,uint32_t instruction,uint32_t target,uint32_t *output);

/* The real dense count remains the public forest bound. Equation and atom
 * identities stay in the independent sealed owner; they never replace it. */
static XrXirStatus effect_context_forest_seal(const XrXirCompileContext *context,
    const XrXirModule *source,EffectOrdinaryContexts *contexts,EffectContextForest **output) {
    if (!xir_compile_context_valid(context) || !source || !contexts || !output || *output ||
        contexts->uses.resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=contexts->uses.root?XR_XIR_OK:effect_ordinary_roots(context,source,contexts);
    if (status==XR_XIR_OK) status=effect_invocation_public_gate(context,contexts->uses.invocations,contexts->count);
    if (status!=XR_XIR_OK) return status;
    EffectContextForest *forest=xir_compile_calloc(context,1,sizeof(*forest),&status);
    if (!forest) return status;
    forest->resources=context->resources;forest->count=contexts->count;forest->terms.remaining=context;
    status=effect_invocation_forest_types(context,contexts,&forest->terms);
    uint64_t records=(uint64_t)forest->count*4;
    if (records>SIZE_MAX/sizeof(*forest->certificates)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        forest->nodes=effect_terms_alloc(&forest->terms,forest->count,sizeof(*forest->nodes));
        forest->facts=effect_terms_alloc(&forest->terms,forest->count,sizeof(*forest->facts));
        forest->witnesses=effect_terms_alloc(&forest->terms,records,sizeof(*forest->witnesses));
        forest->certificates=effect_terms_alloc(&forest->terms,records,sizeof(*forest->certificates));
        if (!forest->nodes || !forest->facts || !forest->witnesses || !forest->certificates)
            status=forest->terms.status;
    }
    for (uint32_t f=0;f<forest->count && status==XR_XIR_OK;++f)
        status=effect_context_forest_node(forest,contexts,f);
    if (status==XR_XIR_OK) status=effect_context_forest_requirements(context,source,contexts,forest);
    if (status==XR_XIR_OK) status=effect_invocation_forest_records(context,forest,contexts);
    if (status!=XR_XIR_OK) { effect_context_forest_free(forest);return status; }
    /* No fallible step follows the actual transfer. The type view stays
     * live until this same retained owner is released by forest_free. */
    forest->invocations=contexts->uses.invocations;contexts->uses.invocations=NULL;
    forest->terms.remaining=NULL;*output=forest;return XR_XIR_OK;
}

static XrXirStatus effect_context_identity_matches(const XrXirCompileContext *context,
    const EffectContextForest *forest, const EffectForestNode *expected, uint32_t actual) {
    if (actual>=forest->count) return XR_XIR_BAD_STRUCTURE;
    const EffectForestNode *node=&forest->nodes[actual];
    if (!xir_compile_work(context,3)) return XR_XIR_BUDGET;
    if (expected->declaration!=node->declaration || expected->argument_count!=node->argument_count ||
        expected->parameter_count!=node->parameter_count || expected->owner!=node->owner) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};XrXirStatus status=XR_XIR_OK;
    for (uint32_t a=0;a<node->argument_count && status==XR_XIR_OK;++a)
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &forest->terms.types,&forest->terms.types,NULL,0,expected->arguments[a],node->arguments[a],&scratch);
    for (uint32_t p=0;p<node->parameter_count && status==XR_XIR_OK;++p)
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            &forest->terms.types,&forest->terms.types,NULL,0,expected->physical_types[p],node->physical_types[p],&scratch);
    xr_xir_type_match_scratch_free(&scratch);
    return status==XR_XIR_BAD_TYPE ? XR_XIR_BAD_STRUCTURE : status;
}

static XrXirStatus effect_context_forest_trace(const XrXirCompileContext *context,
    const EffectContextForest *forest,uint32_t selected,XrXirRootCauseTrace **output) {
    return effect_invocation_forest_trace(context,forest,selected,output);
}
