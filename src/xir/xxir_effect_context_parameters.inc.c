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

static void effect_ordinary_free(EffectOrdinaryContexts *contexts) {
    if (!contexts) return;
    xr_xir_compile_declarations_free(contexts->source_declarations);
    xr_xir_compile_generics_free(contexts->source_generics,contexts->base_count);
    effect_parameters_free(&contexts->uses);
    xr_compile_resources_free(contexts->uses.root);
    xr_compile_resources_free(contexts->uses.root_witnesses);
    xr_compile_resources_free(contexts->uses.unresolved_witnesses);
    xr_compile_resources_free(contexts->uses.functions);
    xr_compile_resources_free(contexts->uses.task_creation);
    effect_terms_free(&contexts->terms);
    xr_compile_resources_free(contexts);
}

static XrXirStatus effect_ordinary_capacity(EffectOrdinaryContexts *contexts, uint32_t needed) {
    if (needed<=contexts->capacity) return XR_XIR_OK;
    uint32_t maximum=contexts->terms.remaining->limits.functions;
    if (needed>maximum) return XR_XIR_BUDGET;
    uint32_t capacity=contexts->capacity<8 ? 8 : contexts->capacity;
    while (capacity<needed) {
        if (!xir_compile_work(contexts->terms.remaining,1)) return XR_XIR_BUDGET;
        if (capacity>maximum/2) { capacity=maximum;break; }
        capacity*=2;
    }
    if (capacity>maximum) capacity=maximum;
    EffectOrdinaryNode *nodes=effect_terms_alloc(&contexts->terms,capacity,sizeof(*nodes));
    XrXirFunction *functions=effect_terms_alloc(&contexts->terms,capacity,sizeof(*functions));
    if (!nodes || !functions) return contexts->terms.status;
    if (!xir_compile_work(contexts->terms.remaining,(uint64_t)contexts->count*
        (sizeof(*nodes)+sizeof(*functions)))) return XR_XIR_BUDGET;
    if (contexts->count) {
        memcpy(nodes,contexts->nodes,(size_t)contexts->count*sizeof(*nodes));
        memcpy(functions,contexts->functions,(size_t)contexts->count*sizeof(*functions));
    }
    contexts->nodes=nodes;contexts->functions=functions;contexts->capacity=capacity;
    return XR_XIR_OK;
}

static XrXirStatus effect_ordinary_function(EffectOrdinaryContexts *contexts,
    const XrXirModule *source, uint32_t index) {
    if (!xir_compile_work(contexts->terms.remaining,
        sizeof(EffectOrdinaryNode)+2*sizeof(XrXirFunction))) return XR_XIR_BUDGET;
    EffectOrdinaryNode identity=contexts->nodes[index];
    XrXirFunction function=source->functions[identity.declaration];
    XrXirGeneric environment={.arguments=identity.arguments,.argument_count=identity.argument_count};
    bool substitute=index>=contexts->base_count;
    XrXirType *parameters=function.parameter_count ?
        effect_terms_alloc(&contexts->terms,function.parameter_count,sizeof(*parameters)) : NULL;
    XrXirInstruction *ops=function.instruction_count ?
        effect_terms_alloc(&contexts->terms,function.instruction_count,sizeof(*ops)) : NULL;
    uint32_t *operands=function.operand_count ?
        effect_terms_alloc(&contexts->terms,function.operand_count,sizeof(*operands)) : NULL;
    if ((function.parameter_count && !parameters) || (function.instruction_count && !ops) ||
        (function.operand_count && !operands)) return contexts->terms.status;
    for (uint32_t p=0;p<function.parameter_count;++p) {
        if (!xir_compile_work(contexts->terms.remaining,1)) return XR_XIR_BUDGET;
        XrXirStatus status=substitute ? effect_terms_substitute(&contexts->terms,
            function.parameters[p],&environment,&parameters[p]) : XR_XIR_OK;
        if (!substitute) parameters[p]=function.parameters[p];
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t i=0;i<function.instruction_count;++i) {
        if (!xir_compile_work(contexts->terms.remaining,sizeof(*ops))) return XR_XIR_BUDGET;
        ops[i]=function.instructions[i];
        if (substitute) {
            XrXirStatus status=effect_terms_substitute(&contexts->terms,ops[i].type,&environment,&ops[i].type);
            if (status!=XR_XIR_OK) return status;
        }
    }
    if (!xir_compile_work(contexts->terms.remaining,(uint64_t)function.operand_count*sizeof(*operands)))
        return XR_XIR_BUDGET;
    if (function.operand_count) memcpy(operands,function.operands,(size_t)function.operand_count*sizeof(*operands));
    if (substitute) {
        XrXirStatus status=effect_terms_substitute(&contexts->terms,function.result,&environment,&function.result);
        if (status!=XR_XIR_OK) return status;
    }
    function.parameters=parameters;function.instructions=ops;function.operands=operands;
    char *name=function.name_length ? effect_terms_alloc(&contexts->terms,function.name_length,1) : NULL;
    if (function.name_length && !name) return contexts->terms.status;
    if (!xir_compile_work(contexts->terms.remaining,function.name_length)) return XR_XIR_BUDGET;
    if (function.name_length) memcpy(name,function.name,function.name_length);
    function.name=name;function.blocks=NULL;function.block_count=0;
    contexts->functions[index]=function;contexts->nodes[index].source_parameters=parameters;
    return XR_XIR_OK;
}

static XrXirStatus effect_ordinary_environment_same(EffectOrdinaryContexts *contexts,
    const EffectOrdinaryNode *node, uint32_t declaration, const XrXirType *arguments,
    uint32_t count, bool *equal) {
    *equal=node->declaration==declaration && node->argument_count==count;
    XrXirTypeMatchScratch scratch={contexts->terms.remaining->resources,NULL};
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t a=0;*equal && a<count && status==XR_XIR_OK;++a) {
        status=xr_xir_compile_type_substitution_matches_between_scratch(contexts->terms.remaining,
            &contexts->terms.types,&contexts->terms.types,NULL,0,node->arguments[a],arguments[a],&scratch);
        if (status==XR_XIR_BAD_TYPE) { *equal=false;status=XR_XIR_OK; }
    }
    xr_xir_type_match_scratch_free(&scratch);return status;
}

static XrXirStatus effect_ordinary_intern(EffectOrdinaryContexts *contexts,
    const XrXirModule *source, const EffectContextResolved *target, uint32_t caller, uint32_t *output) {
    if (!target->known || target->target>=source->function_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t owner=UINT32_MAX;
    if (source->declarations && source->declarations->functions[target->target].cleanup_owner) {
        if (caller>=contexts->count || contexts->nodes[caller].declaration+1!=
            source->declarations->functions[target->target].cleanup_owner) return XR_XIR_BAD_STRUCTURE;
        owner=caller;
    }
    if (!target->argument_count) { *output=target->target;return XR_XIR_OK; }
    const XrXirType *cache=NULL;
    bool local=contexts->terms.types.nodes==target->terms.types.nodes &&
        contexts->terms.types.count==target->terms.types.count;
    XrXirStatus status=local ? XR_XIR_OK : effect_terms_import(&contexts->terms,&target->terms.types,&cache);
    if (status!=XR_XIR_OK) return status;
    XrXirType *arguments=effect_terms_alloc(&contexts->terms,target->argument_count,sizeof(*arguments));
    if (!arguments) return contexts->terms.status;
    for (uint32_t a=0;a<target->argument_count;++a) {
        if (local) {
            if (!xir_compile_work(contexts->terms.remaining,1)) return XR_XIR_BUDGET;
            arguments[a]=target->arguments[a];
        } else status=effect_terms_import_child(&contexts->terms,cache,target->terms.types.count,
            target->arguments[a],&arguments[a]);
        if (status!=XR_XIR_OK) return status;
    }
    for (uint32_t n=contexts->base_count;n<contexts->count;++n) {
        if (!xir_compile_work(contexts->terms.remaining,2)) return XR_XIR_BUDGET;
        EffectOrdinaryNode node=contexts->nodes[n];
        if (node.owner!=owner) continue;
        bool equal=false;
        status=effect_ordinary_environment_same(contexts,&node,target->target,arguments,
            target->argument_count,&equal);
        if (status!=XR_XIR_OK) return status;
        if (equal) { *output=n;return XR_XIR_OK; }
    }
    if (contexts->count==UINT32_MAX) return XR_XIR_BUDGET;
    status=effect_ordinary_capacity(contexts,contexts->count+1);
    if (status!=XR_XIR_OK) return status;
    uint32_t index=contexts->count++;
    contexts->nodes[index]=(EffectOrdinaryNode){.declaration=target->target,.arguments=arguments,
        .argument_count=target->argument_count,.ordinary=index,.owner=owner};
    status=effect_ordinary_function(contexts,source,index);
    if (status==XR_XIR_OK) *output=index;
    return status;
}

static XrXirStatus effect_ordinary_expand(EffectOrdinaryContexts *contexts,
    const XrXirModule *source, uint32_t index) {
    EffectOrdinaryNode identity=contexts->nodes[index];
    const XrXirGeneric *generic=source->generics ? &source->generics[identity.declaration] : NULL;
    if (index<contexts->base_count && generic && generic->parameter_count) return XR_XIR_OK;
    uint32_t count=source->functions[identity.declaration].instruction_count;
    for (uint32_t i=0;i<count;++i) {
        if (!xir_compile_work(contexts->terms.remaining,1)) return XR_XIR_BUDGET;
        XrXirInstruction original=source->functions[identity.declaration].instructions[i];
        if (!effect_context_family(original.op)) continue;
        EffectContextRequest request={source,&contexts->terms.types,identity.arguments,
            identity.argument_count,identity.declaration,i};
        EffectContextResolved target={0};
        XrXirStatus status=effect_context_resolve_in(&contexts->terms,&request,&target);
        if (status!=XR_XIR_OK) return status;
        if (!target.known) continue;
        uint32_t selected=UINT32_MAX;
        status=effect_ordinary_intern(contexts,source,&target,index,&selected);
        if (status!=XR_XIR_OK) return status;
        /* Interning may move the function vector. Reacquire by numeric index. */
        XrXirInstruction *op=(XrXirInstruction *)&contexts->functions[index].instructions[i];
        op->immediate=selected;
        if (original.op==XR_XIR_CALL_REQUIREMENT || original.op==XR_XIR_CALL_DEFAULT) {
            op->op=XR_XIR_CALL;op->targets[0]=op->targets[1]=0;
        } else if (original.op==XR_XIR_INVOKE_DEFAULT) {
            op->op=XR_XIR_INVOKE;op->args[0]=op->args[1]=0;
        }
        op->type_arguments[0]=op->type_arguments[1]=0;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_ordinary_snapshot(EffectOrdinaryContexts *contexts,
    const XrXirModule *source) {
    const XrXirCompileContext *context=contexts->terms.remaining;
    XrXirStatus status=xr_xir_compile_declarations_clone(context,source->declarations,
        source->function_count,&contexts->source_declarations);
    if (status==XR_XIR_OK && source->generics)
        status=xr_xir_compile_generics_clone(context,source,&contexts->source_generics);
    if (status==XR_XIR_OK && source->defaults) {
        uint32_t count=source->defaults->count;
        XrXirDefaultBinding *records=count ? effect_terms_alloc(&contexts->terms,count,sizeof(*records)) : NULL;
        if (count && !records) return contexts->terms.status;
        if (!xir_compile_work(context,(uint64_t)count*sizeof(*records))) return XR_XIR_BUDGET;
        if (count) memcpy(records,source->defaults->records,(size_t)count*sizeof(*records));
        contexts->source_defaults=(XrXirDefaultTable){records,count};
    }
    return status;
}

typedef struct EffectOrdinaryView {
    XrXirModule module;
    XrXirDeclarations declarations;
    XrXirProvenance evidence;
} EffectOrdinaryView;

/* All contextual function metadata uses contextual indices. The source
 * identity relation is verified before any rewritten cleanup owner enters it. */
static XrXirStatus effect_ordinary_view(EffectOrdinaryContexts *contexts,
    const XrXirModule *source, EffectOrdinaryView *output) {
    if (!source->declarations || !source->declarations->functions) return XR_XIR_BAD_STRUCTURE;
    XrXirFunctionIdentity *identities=effect_terms_alloc(&contexts->terms,contexts->count,sizeof(*identities));
    XrXirGeneric *generics=effect_terms_alloc(&contexts->terms,contexts->count,sizeof(*generics));
    if (!identities || !generics) return contexts->terms.status;
    for (uint32_t f=0;f<contexts->count;++f) {
        if (!xir_compile_work(contexts->terms.remaining,sizeof(*identities)+sizeof(*generics)+2))
            return XR_XIR_BUDGET;
        uint32_t declaration=contexts->nodes[f].declaration;
        if (declaration>=source->function_count) return XR_XIR_BAD_STRUCTURE;
        identities[f]=source->declarations->functions[declaration];
        if (f<contexts->base_count && source->generics) generics[f]=source->generics[f];
        if (identities[f].cleanup_owner) {
            uint32_t owner=contexts->nodes[f].owner;
            if (owner>=contexts->count || contexts->nodes[owner].declaration+1!=identities[f].cleanup_owner)
                return XR_XIR_BAD_STRUCTURE;
            identities[f].cleanup_owner=owner+1;
        } else if (contexts->nodes[f].owner!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
    }
    *output=(EffectOrdinaryView){0};output->declarations=*source->declarations;
    output->declarations.functions=identities;output->evidence.kind=XR_XIR_EVIDENCE_TEMPLATE;
    output->module=*source;output->module.functions=contexts->functions;
    output->module.function_count=contexts->count;output->module.types=&contexts->terms.types;
    output->module.generics=generics;output->module.declarations=&output->declarations;
    output->module.provenance=&output->evidence;return XR_XIR_OK;
}

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
    EffectOrdinaryContexts **output, uint32_t *selected) {
    if (!xir_compile_context_valid(context) || !source || !source->types ||
        !source->functions || !source->function_count || !seed || !seed->known ||
        seed->resources!=context->resources || !output || *output || !selected)
        return XR_XIR_BAD_STRUCTURE;
    EffectContextResolved *fresh=NULL;
    XrXirStatus status=effect_ordinary_seed_rebuild(context,source,seed,&fresh);
    if (status!=XR_XIR_OK) return status;
    EffectOrdinaryContexts *contexts=xir_compile_calloc(context,1,sizeof(*contexts),&status);
    if (!contexts) { effect_context_free(fresh);return status; }
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
    contexts->uses.resources=context->resources;contexts->uses.count=contexts->count;
    if (status==XR_XIR_OK) status=effect_ordinary_view(contexts,source,&view);
    if (status==XR_XIR_OK) status=effect_parameters_derive(&view.module,&contexts->uses,&graph,context);
    xr_compile_resources_free(graph.flow_rows);
    if (status!=XR_XIR_OK) { effect_ordinary_free(contexts);return status; }
    contexts->terms.remaining=NULL;
    *selected=target;*output=contexts;return XR_XIR_OK;
}

#endif

static XrXirStatus effect_ordinary_build(const XrXirCompileContext *context,
    const XrXirModule *source, EffectOrdinaryContexts **output) {
    if (!xir_compile_context_valid(context) || !source || !source->types ||
        !source->functions || !source->function_count || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    EffectOrdinaryContexts *contexts=xir_compile_calloc(context,1,sizeof(*contexts),&status);
    if (!contexts) return status;
    contexts->terms.remaining=context;contexts->base_count=source->function_count;
    contexts->source_type_count=source->types->count;
    status=effect_terms_seed(&contexts->terms,source->types);
    if (status==XR_XIR_OK) status=effect_ordinary_snapshot(contexts,source);
    if (status==XR_XIR_OK) status=effect_ordinary_capacity(contexts,source->function_count);
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        contexts->nodes[f]=(EffectOrdinaryNode){.declaration=f,.ordinary=f,.owner=UINT32_MAX};
        contexts->count=f+1;
        if (source->declarations && source->declarations->functions[f].cleanup_owner)
            contexts->nodes[f].owner=source->declarations->functions[f].cleanup_owner-1;
        status=effect_ordinary_function(contexts,source,f);
    }
    for (uint32_t f=0;f<contexts->count && status==XR_XIR_OK;++f)
        status=effect_ordinary_expand(contexts,source,f);
    EffectGraph graph={0};EffectOrdinaryView view={0};
    contexts->uses.resources=context->resources;contexts->uses.count=contexts->count;
    if (status==XR_XIR_OK) status=effect_ordinary_view(contexts,source,&view);
    if (status==XR_XIR_OK) status=effect_parameters_derive(&view.module,&contexts->uses,&graph,context);
    xr_compile_resources_free(graph.flow_rows);
    if (status!=XR_XIR_OK) { effect_ordinary_free(contexts);return status; }
    contexts->terms.remaining=NULL;*output=contexts;return XR_XIR_OK;
}
