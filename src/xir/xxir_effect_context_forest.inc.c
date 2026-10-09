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
    EffectTerms terms;
    XrCompileResources *resources;
    uint32_t count;
    EffectForestNode *nodes;
    XrXirRootEffects *facts;
    XrXirRootEffectWitness *witnesses;
    EffectContextCertificate *certificates;
} EffectContextForest;

static void effect_context_forest_free(EffectContextForest *forest) {
    if (!forest) return;
    effect_terms_free(&forest->terms);xr_compile_resources_free(forest);
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

static XrXirStatus effect_context_forest_record(EffectContextForest *forest,
    const EffectOrdinaryContexts *contexts, const XrXirModule *source,
    uint32_t domain, uint32_t function) {
    bool unresolved=(domain&1)!=0,constant=(domain&2)!=0;
    const XrXirRootEffectWitness *records=constant ?
        contexts->uses.constant_witnesses+(size_t)unresolved*forest->count :
        unresolved ? contexts->uses.unresolved_witnesses : contexts->uses.root_witnesses;
    XrXirRootEffectWitness raw=records[function],public_value=raw;
    uint32_t declaration=forest->nodes[function].declaration;
    const XrXirFunction *original=&source->functions[declaration];
    uint32_t next=UINT32_MAX,parameter_owner=function;
    if (!xir_compile_work(forest->terms.remaining,7)) return XR_XIR_BUDGET;
    if (raw.callee!=UINT32_MAX) {
        if (raw.callee>=forest->count) return XR_XIR_BAD_STRUCTURE;
        public_value.callee=forest->nodes[raw.callee].declaration;parameter_owner=raw.callee;
    }
    if (raw.distance) {
        if ((raw.cause!=XR_XIR_ROOT_CAUSE_CALL && raw.cause!=XR_XIR_ROOT_CAUSE_CLEANUP) ||
            raw.callee>=forest->count || raw.instruction>=original->instruction_count ||
            raw.slot!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
        next=raw.callee;
        XrXirOp operation=original->instructions[raw.instruction].op;
        if (operation==XR_XIR_CALL_REQUIREMENT) {
            if (raw.cause!=XR_XIR_ROOT_CAUSE_CALL ||
                contexts->functions[function].instructions[raw.instruction].op!=XR_XIR_CALL ||
                contexts->functions[function].instructions[raw.instruction].immediate!=raw.callee)
                return XR_XIR_BAD_STRUCTURE;
            public_value.cause=XR_XIR_ROOT_CAUSE_REQUIREMENT;
        }
    } else if (raw.cause==XR_XIR_ROOT_CAUSE_PARAMETER || raw.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL ||
        raw.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS || raw.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER) {
        if (!xir_compile_work(forest->terms.remaining,5)) return XR_XIR_BUDGET;
        const XrXirRootEffectWitness *proof=&contexts->uses.formula_terminals[(size_t)domain*forest->count+function];
        if (!effect_context_witness_same(&raw,proof)) return XR_XIR_BAD_STRUCTURE;
        if (raw.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL && !unresolved) return XR_XIR_BAD_STRUCTURE;
        if (raw.cause==XR_XIR_ROOT_CAUSE_PARAMETER &&
            (raw.slot>=forest->nodes[parameter_owner].parameter_count ||
             forest->nodes[parameter_owner].parameter_kinds[raw.slot]!=XR_XIR_EFFECT_PARAMETER_VARIABLE))
            return XR_XIR_BAD_STRUCTURE;
        if (raw.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER &&
            (raw.callee!=UINT32_MAX || raw.slot>=forest->nodes[parameter_owner].parameter_count ||
             !xr_xir_type_is_cell(&forest->terms.types,forest->nodes[parameter_owner].physical_types[raw.slot])))
            return XR_XIR_BAD_STRUCTURE;
        if (raw.cause==XR_XIR_ROOT_CAUSE_CELL_ACCESS &&
            (raw.callee!=UINT32_MAX || raw.instruction>=original->instruction_count ||
             raw.slot>=(uint64_t)original->parameter_count+original->instruction_count))
            return XR_XIR_BAD_STRUCTURE;
    } else if (raw.cause==XR_XIR_ROOT_CAUSE_REQUIREMENT &&
        (raw.instruction>=original->instruction_count ||
         original->instructions[raw.instruction].op!=XR_XIR_CALL_REQUIREMENT ||
         raw.callee!=UINT32_MAX)) return XR_XIR_BAD_STRUCTURE;
    size_t index=(size_t)domain*forest->count+function;
    if (!xir_compile_work(forest->terms.remaining,
        sizeof(*forest->witnesses)+sizeof(*forest->certificates))) return XR_XIR_BUDGET;
    forest->witnesses[index]=public_value;
    uint32_t target=next==UINT32_MAX ? parameter_owner : next;
    forest->certificates[index]=(EffectContextCertificate){public_value,next,parameter_owner,
        (uint8_t)domain,(uint8_t)(2+(uint32_t)unresolved),forest->nodes[function],forest->nodes[target]};
    return XR_XIR_OK;
}

/* The shared solver closes actual dense views before their exact identities
 * and selected-domain witnesses enter this independent certificate owner. */
static XrXirStatus effect_context_forest_seal(const XrXirCompileContext *context,
    const XrXirModule *source, EffectOrdinaryContexts *contexts, EffectContextForest **output) {
    if (!xir_compile_context_valid(context) || !source || !contexts || !output || *output ||
        contexts->uses.resources!=context->resources) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=contexts->uses.root ? XR_XIR_OK : effect_ordinary_roots(context,source,contexts);
    if (status!=XR_XIR_OK) return status;
    EffectContextForest *forest=xir_compile_calloc(context,1,sizeof(*forest),&status);
    if (!forest) return status;
    forest->resources=context->resources;forest->count=contexts->count;forest->terms.remaining=context;
    status=effect_terms_seed(&forest->terms,&contexts->terms.types);
    uint64_t records=(uint64_t)forest->count*4;
    if (records>SIZE_MAX/sizeof(*forest->certificates)) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        forest->nodes=effect_terms_alloc(&forest->terms,forest->count,sizeof(*forest->nodes));
        forest->facts=effect_terms_alloc(&forest->terms,forest->count,sizeof(*forest->facts));
        forest->witnesses=effect_terms_alloc(&forest->terms,(size_t)records,sizeof(*forest->witnesses));
        forest->certificates=effect_terms_alloc(&forest->terms,(size_t)records,sizeof(*forest->certificates));
        if (!forest->nodes || !forest->facts || !forest->witnesses || !forest->certificates)
            status=forest->terms.status;
    }
    for (uint32_t f=0;f<forest->count && status==XR_XIR_OK;++f)
        status=effect_context_forest_node(forest,contexts,f);
    for (uint32_t domain=0;domain<4 && status==XR_XIR_OK;++domain)
        for (uint32_t f=0;f<forest->count && status==XR_XIR_OK;++f)
            status=effect_context_forest_record(forest,contexts,source,domain,f);
    if (status!=XR_XIR_OK) { effect_context_forest_free(forest);return status; }
    forest->terms.remaining=NULL;*output=forest;return XR_XIR_OK;
}

static bool effect_context_forest_fact(const EffectContextForest *forest, uint32_t context,
    uint32_t domain) {
    uint32_t bit=(domain&1) ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : XR_XIR_CALLABLE_ROOT_REQUIRED;
    return (domain&2) ? (forest->nodes[context].constant_mask&bit)!=0 :
        (domain&1) ? forest->facts[context].unresolved : forest->facts[context].requires_root;
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

/* The private next context is independent of the public declaration ordinal.
 * A copied step is rechecked against the certificate for this exact domain. */
static XrXirStatus effect_context_forest_walk(const XrXirCompileContext *context,
    const EffectContextForest *forest, uint32_t selected, bool unresolved,
    uint32_t *length, XrXirRootCauseStep *steps) {
    uint32_t current=selected,domain=(uint32_t)unresolved,count=0;
    if (!effect_context_forest_fact(forest,current,domain)) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (forest->witnesses[(size_t)domain*forest->count+current].cause) return XR_XIR_BAD_STRUCTURE;
        *length=0;return XR_XIR_OK;
    }
    for (;;) {
        if (!xir_compile_work(context,10)) return XR_XIR_BUDGET;
        size_t index=(size_t)domain*forest->count+current;
        const EffectContextCertificate *certificate=&forest->certificates[index];
        XrXirRootEffectWitness witness=forest->witnesses[index];
        if (count>=forest->count || witness.distance>=forest->count || certificate->domain!=domain ||
            !effect_context_forest_fact(forest,current,domain) ||
            !effect_context_witness_same(&witness,&certificate->witness)) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus identity=effect_context_identity_matches(context,forest,&certificate->subject,current);
        if (identity!=XR_XIR_OK) return identity;
        if (steps) {
            if (!xir_compile_work(context,sizeof(*steps))) return XR_XIR_BUDGET;
            steps[count]=(XrXirRootCauseStep){forest->nodes[current].declaration,witness.instruction,
                witness.callee,witness.slot,witness.distance,witness.cause};
        }
        ++count;
        if (!witness.distance) {
            if (certificate->next!=UINT32_MAX || !witness.cause ||
                (witness.cause==XR_XIR_ROOT_CAUSE_CONTEXT_CALL && !unresolved))
                return XR_XIR_BAD_STRUCTURE;
            if (witness.cause==XR_XIR_ROOT_CAUSE_PARAMETER) {
                identity=effect_context_identity_matches(context,forest,&certificate->target,
                    certificate->parameter_owner);
                if (identity!=XR_XIR_OK) return identity;
                const EffectForestNode *owner=&forest->nodes[certificate->parameter_owner];
                if (witness.slot>=owner->parameter_count ||
                    owner->parameter_kinds[witness.slot]!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                    return XR_XIR_BAD_STRUCTURE;
            }
            if (witness.cause==XR_XIR_ROOT_CAUSE_CELL_PARAMETER) {
                identity=effect_context_identity_matches(context,forest,&certificate->target,
                    certificate->parameter_owner);
                if (identity!=XR_XIR_OK) return identity;
                const EffectForestNode *owner=&forest->nodes[certificate->parameter_owner];
                if (witness.slot>=owner->parameter_count ||
                    !xr_xir_type_is_cell(&forest->terms.types,owner->physical_types[witness.slot]))
                    return XR_XIR_BAD_STRUCTURE;
            }
            *length=count;return XR_XIR_OK;
        }
        if ((witness.cause!=XR_XIR_ROOT_CAUSE_CALL && witness.cause!=XR_XIR_ROOT_CAUSE_CLEANUP &&
            witness.cause!=XR_XIR_ROOT_CAUSE_REQUIREMENT) || certificate->next>=forest->count ||
            certificate->next_domain!=(uint8_t)(2+(uint32_t)unresolved) ||
            witness.callee!=forest->nodes[certificate->next].declaration) return XR_XIR_BAD_STRUCTURE;
        identity=effect_context_identity_matches(context,forest,&certificate->target,certificate->next);
        if (identity!=XR_XIR_OK) return identity;
        size_t next=(size_t)certificate->next_domain*forest->count+certificate->next;
        if (forest->witnesses[next].distance!=witness.distance-1) return XR_XIR_BAD_STRUCTURE;
        current=certificate->next;domain=certificate->next_domain;
    }
}

static XrXirStatus effect_context_forest_trace(const XrXirCompileContext *context,
    const EffectContextForest *forest, uint32_t selected, XrXirRootCauseTrace **output) {
    if (!xir_compile_context_valid(context) || !forest || forest->resources!=context->resources ||
        selected>=forest->count || !output || *output) return XR_XIR_BAD_STRUCTURE;
    uint32_t counts[2]={0};XrXirStatus status=effect_context_forest_walk(context,forest,selected,false,&counts[0],NULL);
    if (status==XR_XIR_OK) status=effect_context_forest_walk(context,forest,selected,true,&counts[1],NULL);
    if (status!=XR_XIR_OK) return status;
    uint64_t total=(uint64_t)counts[0]+counts[1];
    if (total>(SIZE_MAX-sizeof(XrXirRootCauseTrace))/sizeof(XrXirRootCauseStep)) return XR_XIR_BUDGET;
    size_t bytes=sizeof(XrXirRootCauseTrace)+(size_t)total*sizeof(XrXirRootCauseStep);
    XrXirRootCauseTrace *trace=xir_compile_calloc(context,1,bytes,&status);
    if (!trace) return status;
    XrXirRootCauseStep *steps=effect_root_trace_storage(trace);
    uint32_t copied=0;
    status=effect_context_forest_walk(context,forest,selected,false,&copied,steps);
    if (status==XR_XIR_OK && copied!=counts[0]) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK)
        status=effect_context_forest_walk(context,forest,selected,true,&copied,steps+counts[0]);
    if (status==XR_XIR_OK && copied!=counts[1]) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_xir_compile_root_cause_trace_free(trace);return status; }
    if (!xir_compile_work(context,sizeof(*trace))) {
        xr_xir_compile_root_cause_trace_free(trace);return XR_XIR_BUDGET;
    }
    trace->facts=forest->facts[selected];trace->counts[0]=counts[0];trace->counts[1]=counts[1];
    *output=trace;return XR_XIR_OK;
}
