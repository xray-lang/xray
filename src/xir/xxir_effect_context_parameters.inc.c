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
    xr_compile_resources_free(contexts->nodes);
    xr_compile_resources_free(contexts->functions);
    effect_terms_free(&contexts->terms);
    xr_compile_resources_free(contexts);
}

/* Descriptor vectors are replaceable metadata, not permanent term payloads.
 * Each root belongs to the same finite ledger. Every admitted byte is still
 * initialized; growth publishes only after both complete vectors are copied. */
static void *effect_ordinary_descriptor_alloc(EffectOrdinaryContexts *contexts,
    uint32_t count, size_t size) {
    EffectTerms *terms=&contexts->terms;
    if (terms->status!=XR_XIR_OK) return NULL;
    if (!size || count>SIZE_MAX/size ||
        !xir_compile_work(terms->remaining,(uint64_t)count+1)) {
        terms->status=XR_XIR_BUDGET;return NULL;
    }
    return xir_compile_calloc(terms->remaining,count,size,&terms->status);
}

static XrXirStatus effect_ordinary_capacity(EffectOrdinaryContexts *contexts, uint32_t needed) {
    if (contexts->terms.status!=XR_XIR_OK) return contexts->terms.status;
    if (needed<=contexts->capacity) return XR_XIR_OK;
    uint32_t maximum=effect_body_entry_limit(contexts->terms.remaining);
    if (needed>maximum) { contexts->terms.status=XR_XIR_BUDGET;return contexts->terms.status; }
    uint32_t capacity=contexts->capacity<8 ? 8 : contexts->capacity;
    while (capacity<needed) {
        if (!xir_compile_work(contexts->terms.remaining,1)) {
            contexts->terms.status=XR_XIR_BUDGET;return contexts->terms.status;
        }
        if (capacity>maximum/2) { capacity=maximum;break; }
        capacity*=2;
    }
    if (capacity>maximum) capacity=maximum;
    EffectOrdinaryNode *nodes=effect_ordinary_descriptor_alloc(contexts,capacity,sizeof(*nodes));
    XrXirFunction *functions=contexts->terms.status==XR_XIR_OK ?
        effect_ordinary_descriptor_alloc(contexts,capacity,sizeof(*functions)) : NULL;
    if (!nodes || !functions) {
        xr_compile_resources_free(nodes);xr_compile_resources_free(functions);
        return contexts->terms.status;
    }
    if (!xir_compile_work(contexts->terms.remaining,(uint64_t)contexts->count*
        (sizeof(*nodes)+sizeof(*functions)))) {
        xr_compile_resources_free(nodes);xr_compile_resources_free(functions);
        contexts->terms.status=XR_XIR_BUDGET;return contexts->terms.status;
    }
    if (contexts->count) {
        memcpy(nodes,contexts->nodes,(size_t)contexts->count*sizeof(*nodes));
        memcpy(functions,contexts->functions,(size_t)contexts->count*sizeof(*functions));
    }
    xr_compile_resources_free(contexts->nodes);xr_compile_resources_free(contexts->functions);
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
        effect_terms_reserve(&contexts->terms,function.instruction_count,sizeof(*ops)) : NULL;
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
    /* Lookup compares the actual input pool before taking another environment
     * copy. Only a genuinely new key owns imported argument storage. */
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t n=contexts->base_count;n<contexts->count;++n) {
        if (!xir_compile_work(contexts->terms.remaining,2)) return XR_XIR_BUDGET;
        EffectOrdinaryNode node=contexts->nodes[n];
        if (node.owner!=owner) continue;
        bool equal=false;
        status=effect_ordinary_environment_same(&contexts->terms,&node,target,&equal);
        if (status!=XR_XIR_OK) return status;
        if (equal) { *output=n;return XR_XIR_OK; }
    }
    const XrXirType *cache=NULL;
    bool local=contexts->terms.types.nodes==target->terms.types.nodes &&
        contexts->terms.types.count==target->terms.types.count;
    status=local ? XR_XIR_OK : effect_terms_import(&contexts->terms,&target->terms.types,&cache);
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
    if (!xir_compile_work(work,13)) return XR_XIR_BUDGET;
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
        if (!xir_compile_work(work,9+sizeof(XrXirFunctionIdentity)+sizeof(XrXirGeneric))) return XR_XIR_BUDGET;
        const EffectOrdinaryNode *node=&contexts->nodes[f];const XrXirFunction *function=&contexts->functions[f];
        if (node->declaration>=source->function_count || !!node->arguments!=!!node->argument_count ||
            (f<contexts->base_count && node->declaration!=f) ||
            function->parameter_count!=source->functions[node->declaration].parameter_count ||
            (function->parameter_count && (!function->parameters || !node->source_parameters ||
             (contexts->dense && !node->physical_types)))) return XR_XIR_BAD_STRUCTURE;
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
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
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
    if (!xir_compile_work(work,2+sizeof(*output))) return XR_XIR_BUDGET;
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
        if (!xir_compile_work(work,8)) return XR_XIR_BUDGET;
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
    if (!xir_compile_work(work,12+sizeof(*output))) return XR_XIR_BUDGET;
    XrXirStatus status=effect_ordinary_basis_source(work,input->basis,input->source);
    if (status!=XR_XIR_OK) return status;
    *output=basis->uses.generation;return XR_XIR_OK;
}

typedef struct EffectOrdinaryView {
    XrXirModule module;
    XrXirDeclarations declarations;
    EffectBodyOrdinaryInput input;
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
    output->declarations.functions=identities;output->input=(EffectBodyOrdinaryInput){.contexts=contexts,.source=source};
    output->module=*source;output->module.functions=contexts->functions;
    output->module.function_count=contexts->count;output->module.types=&contexts->terms.types;
    output->module.generics=generics;output->module.declarations=&output->declarations;
    output->module.provenance=NULL;return XR_XIR_OK;
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
    if (source->function_count>context->limits.functions) return XR_XIR_BUDGET;
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

static XrXirStatus effect_ordinary_build(const XrXirCompileContext *context,
    const XrXirModule *source,EffectBodyMaterial *initial,EffectOrdinaryContexts **output) {
    if (!xir_compile_context_valid(context) || !source || !source->types ||
        !source->functions || !source->function_count || !output || *output ||
        (initial && (!effect_body_material_shape(initial) || initial->resources!=context->resources)))
        return XR_XIR_BAD_STRUCTURE;
    if (source->function_count>context->limits.functions) return XR_XIR_BUDGET;
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
    if (status==XR_XIR_OK && !xir_compile_work(context,sizeof(view.input.initial))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) {
        view.input.initial=initial;graph.ordinary=view.input;
        status=effect_parameters_derive(&view.module,&contexts->uses,&graph,context);
    }
    effect_graph_free(&graph);
    if (status!=XR_XIR_OK) { effect_ordinary_free(contexts);return status; }
    contexts->terms.remaining=NULL;*output=contexts;return XR_XIR_OK;
}
