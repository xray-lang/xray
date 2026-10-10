/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_owner.inc.c - Actual dense contexts in one summary owner
 *
 * KEY CONCEPT:
 *   Authentic source bodies choose ordinary implementations and physical-use
 *   roles before complete actual types enter private execution equations.
 */
struct EffectContextOwner {
    EffectOrdinaryContexts *ordinary, *dense;
    EffectContextForest *forest;
    EffectInvocationDeclaredBounds *declared;
    EffectRefinementBounds *refinement;
};

static void effect_context_owner_free(EffectContextOwner *owner) {
    if (!owner) return;
    effect_context_forest_free(owner->forest);
    effect_invocation_bounds_free(owner->declared);
    effect_ordinary_free(owner->dense);effect_ordinary_free(owner->ordinary);
    xr_compile_resources_free(owner);
}

static void effect_context_summary_clear(EffectOrdinaryContexts *contexts) {
    effect_parameters_free(&contexts->uses);
    xr_compile_resources_free(contexts->uses.functions);
    xr_compile_resources_free(contexts->uses.task_creation);
    xr_compile_resources_free(contexts->uses.root);
    xr_compile_resources_free(contexts->uses.root_witnesses);
    xr_compile_resources_free(contexts->uses.unresolved_witnesses);
    contexts->uses=(XrXirEffects){.resources=contexts->terms.remaining->resources,.count=contexts->count};
}


/* A true CALL_BIND COPY keeps the original consuming declaration after
 * Source discards its temporary refinement owner. Other records describe
 * their current authentic producer and cannot invent a stronger bound. */
static XrXirStatus effect_invocation_source_declarations(const XrXirCompileContext *work,
    const XrXirModule *source,uint32_t function,const XrXirType *prebottom,XrXirType **output) {
    if (!source || !source->functions || function>=source->function_count || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance=source->provenance;
    if (!provenance || provenance->kind!=XR_XIR_EVIDENCE_TEMPLATE) return XR_XIR_OK;
    /* Internal ordinary views carry a marker without stored source records.
     * Their actual types still undergo complete bounds and equation proof. */
    if (!provenance->contracts && !provenance->contract_count) return XR_XIR_OK;
    if (!provenance->contracts || provenance->contract_count!=source->function_count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *body=&source->functions[function];
    const XrXirFunctionEffectContract *contract=&provenance->contracts[function];
    XrXirStatus status=effect_contract_values(work,source,function,contract);
    bool present=false;
    for (uint32_t v=0;v<contract->value_count && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(work,1)) { status=XR_XIR_BUDGET;break; }
        if (contract->values[v].mode==XR_XIR_EFFECT_VALUE_CALL_BIND) present=true;
    }
    if (status!=XR_XIR_OK || !present) return status;
    uint64_t bytes=(uint64_t)body->instruction_count*sizeof(XrXirType);
    if (bytes>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirType *types=xir_compile_alloc(work,(size_t)bytes,&status);
    if (!types) return status;
    for (uint32_t i=0;i<body->instruction_count && status==XR_XIR_OK;++i) {
        if (!xir_compile_work(work,sizeof(*types)+1)) { status=XR_XIR_BUDGET;break; }
        types[i]=prebottom?prebottom[i]:body->instructions[i].type;
    }
    for (uint32_t v=0;v<contract->value_count && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
        const XrXirRootValueIdentity *identity=&contract->values[v];
        if (identity->mode!=XR_XIR_EFFECT_VALUE_CALL_BIND) continue;
        status=xr_xir_compile_callable_weakening(work,source->types,
            body->instructions[identity->instruction].type,identity->declared_type);
        if (status==XR_XIR_OK) types[identity->instruction]=identity->declared_type;
    }
    if (status!=XR_XIR_OK) { xr_compile_resources_free(types);return status; }
    *output=types;return XR_XIR_OK;
}

/* Refinement matched the immutable source type prefix and copied declaration
 * domain before the dense pool was seeded. These original IDs therefore name
 * that verified prefix. Actual ordinary binders still undergo full substitution. */
static XrXirStatus effect_invocation_context_declarations(EffectOrdinaryContexts *dense,
    const XrXirModule *source,const EffectRefinementBounds *bounds,uint32_t index,const XrXirType **output) {
    if (!dense || !output || *output || index>=dense->count)
        return XR_XIR_BAD_STRUCTURE;
    EffectOrdinaryNode node=dense->nodes[index];
    const XrXirFunction *function=&dense->functions[index];
    if (!source || node.declaration>=source->function_count ||
        (bounds && (bounds->resources!=dense->terms.remaining->resources ||
        node.declaration>=bounds->count || bounds->source_type_count>dense->source_type_count)) ||
        dense->source_type_count>dense->terms.types.count ||
        !!node.arguments!=!!node.argument_count) return XR_XIR_BAD_STRUCTURE;
    const EffectRefinementFunction *original=bounds?&bounds->functions[node.declaration]:NULL;
    if (original && (original->instruction_count!=function->instruction_count ||
        original->parameter_count!=function->parameter_count ||
        (original->instruction_count && !original->instructions))) return XR_XIR_BAD_STRUCTURE;
    XrXirType *source_types=NULL;
    XrXirStatus status=effect_invocation_source_declarations(dense->terms.remaining,
        source,node.declaration,original?original->instructions:NULL,&source_types);
    if (status!=XR_XIR_OK || (!original && !source_types)) return status;
    uint32_t count=function->instruction_count;
    XrXirType *types=count?effect_terms_alloc(&dense->terms,count,sizeof(*types)):NULL;
    if (count && !types) { xr_compile_resources_free(source_types);return dense->terms.status; }
    XrXirGeneric environment={.arguments=node.arguments,.argument_count=node.argument_count};
    for (uint32_t i=0;i<count && status==XR_XIR_OK;++i) {
        if (!xir_compile_work(dense->terms.remaining,sizeof(*types)+1)) { status=XR_XIR_BUDGET;break; }
        types[i]=source_types?source_types[i]:original->instructions[i];
        if (index>=dense->base_count) {
            status=effect_terms_substitute(&dense->terms,types[i],&environment,&types[i]);
        }
        if (status==XR_XIR_OK && xr_xir_callable_signature(&dense->terms.types,types[i]))
            status=xr_xir_compile_callable_weakening(dense->terms.remaining,&dense->terms.types,
                function->instructions[i].type,types[i]);
    }
    xr_compile_resources_free(source_types);
    if (status!=XR_XIR_OK) return status;
    *output=types;return XR_XIR_OK;
}

/* Ordinary views carry a TEMPLATE marker for actual physical-use derivation,
 * not stored source records. Capture each authentic source advertisement in
 * this exact ordinary pool before the common queue receives that view. */
static XrXirStatus effect_invocation_context_bounds(const XrXirCompileContext *work,
    const XrXirModule *source,EffectOrdinaryContexts *contexts,EffectInvocationDeclaredBounds **output) {
    if (!contexts || !output || *output || contexts->terms.remaining!=work ||
        contexts->uses.resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationDeclaredBounds *bounds=NULL;
    XrXirStatus status=effect_invocation_bounds_new(work,&bounds);
    for (uint32_t f=0;f<contexts->count && status==XR_XIR_OK;++f) {
        const XrXirType *instructions=NULL;
        status=effect_invocation_context_declarations(contexts,source,NULL,f,&instructions);
        if (status==XR_XIR_OK) status=effect_invocation_bounds_capture(work,bounds,
            &contexts->terms.types,&contexts->functions[f],f,instructions);
    }
    if (status!=XR_XIR_OK) { effect_invocation_bounds_free(bounds);return status; }
    *output=bounds;return XR_XIR_OK;
}

static XrXirStatus effect_context_dense_function(EffectContextOwner *owner,
    const XrXirModule *source, uint32_t index) {
    EffectOrdinaryContexts *dense=owner->dense;
    XrXirStatus status=effect_ordinary_function(dense,source,index);
    if (status!=XR_XIR_OK) return status;
    const XrXirType *physical=dense->nodes[index].physical_types;
    uint32_t count=dense->functions[index].parameter_count;
    if (!physical && count) return XR_XIR_BAD_STRUCTURE;
    XrXirType *parameters=(XrXirType *)dense->functions[index].parameters;
    for (uint32_t p=0;p<count;++p) {
        if (!xir_compile_work(dense->terms.remaining,1)) return XR_XIR_BUDGET;
        parameters[p]=physical[p];
    }
    const XrXirType *declared_instructions=NULL;
    status=effect_invocation_context_declarations(dense,source,owner->refinement,index,&declared_instructions);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_capture(dense->terms.remaining,owner->declared,
        &dense->terms.types,&dense->functions[index],index,declared_instructions);
    if (status!=XR_XIR_OK) return status;
    /* Authentic latent equations start at their private lattice bottom. An
     * advertised UNKNOWN must not seed its own recursive capture equation.
     * No caller can consume this state until the owner is fully solved. */
    XrXirInstruction *instructions=(XrXirInstruction *)dense->functions[index].instructions;
    for (uint32_t i=0;i<dense->functions[index].instruction_count;++i) {
        if (!xir_compile_work(dense->terms.remaining,1)) return XR_XIR_BUDGET;
        if (instructions[i].op!=XR_XIR_FUNCTION_REF) continue;
        const XrXirTypeNode *signature=xr_xir_callable_signature(&dense->terms.types,instructions[i].type);
        if (!signature) return XR_XIR_BAD_TYPE;
        XrXirTypeNode bottom=*signature;
        bottom.flags=(bottom.flags&XR_XIR_CALLABLE_NO_SUSPEND)|XR_XIR_CALLABLE_ROOT_NONE;
        instructions[i].type=effect_term_intern(&dense->terms,bottom);
        if (dense->terms.status!=XR_XIR_OK) return dense->terms.status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_context_dense_intern(EffectContextOwner *owner,
    const XrXirModule *source, uint32_t ordinary, const EffectDenseVector *vector,
    uint32_t caller, uint32_t *output) {
    EffectOrdinaryContexts *dense=owner->dense;
    const EffectOrdinaryNode *original=&owner->ordinary->nodes[ordinary];
    uint32_t count=source->functions[original->declaration].parameter_count;
    if (vector->terms!=&dense->terms || vector->count!=count ||
        (!!count!=!!vector->physical)) return XR_XIR_BAD_STRUCTURE;
    /* This array already belongs to this exact owner and type pool. Its
     * IDs cannot name another numeric declaration domain. */
    const XrXirType *physical=vector->physical;
    XrXirStatus status=XR_XIR_OK;
    uint32_t parent=source->declarations->functions[original->declaration].cleanup_owner;
    uint32_t owner_index=UINT32_MAX;
    if (parent) {
        if (caller>=dense->count || dense->nodes[caller].declaration+1!=parent)
            return XR_XIR_BAD_STRUCTURE;
        owner_index=caller;
    }
    XrXirTypeMatchScratch *scratch=effect_terms_type_scratch(&dense->terms);
    for (uint32_t n=0;n<dense->count && status==XR_XIR_OK;++n) {
        if (!xir_compile_work(dense->terms.remaining,3)) { status=XR_XIR_BUDGET;break; }
        if (dense->nodes[n].ordinary!=ordinary || dense->nodes[n].owner!=owner_index) continue;
        bool equal=true;
        for (uint32_t p=0;p<count && equal;++p) {
            if (!xir_compile_work(dense->terms.remaining,1)) { status=XR_XIR_BUDGET;break; }
            /* Identical immutable IDs in the same verified pool are exact;
             * different IDs still undergo complete structural matching. */
            if (dense->nodes[n].physical_types[p]==physical[p]) continue;
            status=xr_xir_compile_type_substitution_matches_between_scratch(dense->terms.remaining,
                &dense->terms.types,&dense->terms.types,NULL,0,dense->nodes[n].physical_types[p],physical[p],scratch);
            if (status==XR_XIR_BAD_TYPE) { equal=false;status=XR_XIR_OK; }
            else if (status!=XR_XIR_OK) break;
        }
        if (status==XR_XIR_OK && equal) { *output=n;return XR_XIR_OK; }
    }
    if (status!=XR_XIR_OK) return status;
    if (dense->count==UINT32_MAX) return XR_XIR_BUDGET;
    status=effect_ordinary_capacity(dense,dense->count+1);
    if (status!=XR_XIR_OK) return status;
    uint32_t index=dense->count++;
    dense->nodes[index]=*original;
    if (original->argument_count) {
        XrXirType *arguments=effect_terms_alloc(&dense->terms,original->argument_count,sizeof(*arguments));
        if (!arguments) return dense->terms.status;
        for (uint32_t a=0;a<original->argument_count;++a) {
            if (!xir_compile_work(dense->terms.remaining,1)) return XR_XIR_BUDGET;
            arguments[a]=original->arguments[a];
        }
        dense->nodes[index].arguments=arguments;
    }
    dense->nodes[index].ordinary=ordinary;
    dense->nodes[index].owner=owner_index;dense->nodes[index].physical_types=physical;
    status=effect_context_dense_function(owner,source,index);
    if (status==XR_XIR_OK) *output=index;
    return status;
}

static XrXirStatus effect_context_dense_actuals(EffectContextOwner *owner,
    const XrXirModule *source, uint32_t index, uint8_t **fixed_output, bool *changed) {
    EffectOrdinaryContexts *dense=owner->dense;
    XrXirFunction *function=&dense->functions[index];
    uint64_t count=(uint64_t)function->parameter_count+function->instruction_count;
    if (count>UINT32_MAX) return XR_XIR_BUDGET;
    XrXirType *declared=effect_terms_alloc(&dense->terms,(size_t)count,sizeof(*declared));
    XrXirType *values=effect_terms_alloc(&dense->terms,(size_t)count,sizeof(*values));
    XrXirType *before=effect_terms_alloc(&dense->terms,function->instruction_count,sizeof(*before));
    uint8_t *fixed=effect_terms_alloc(&dense->terms,(size_t)count,sizeof(*fixed));
    if ((count && (!declared || !values || !fixed)) || (function->instruction_count && !before)) return dense->terms.status;
    for (uint32_t v=0;v<count;++v) {
        if (!xir_compile_work(dense->terms.remaining,3)) return XR_XIR_BUDGET;
        declared[v]=values[v]=xr_xir_operand_type(function,v);
        if (v>=function->parameter_count) {
            before[v-function->parameter_count]=declared[v];
            XrXirOp operation=function->instructions[v-function->parameter_count].op;
            const XrXirTypeNode *signature=xr_xir_callable_signature(&dense->terms.types,declared[v]);
            bool carrier=operation==XR_XIR_FUNCTION_REF || operation==XR_XIR_COPY ||
                operation==XR_XIR_LOCAL_NEW || operation==XR_XIR_LOCAL_READ || operation==XR_XIR_PHI;
            if (signature && carrier) {
                XrXirTypeNode bound=*signature;
                bound.flags=(bound.flags&XR_XIR_CALLABLE_NO_SUSPEND)|XR_XIR_CALLABLE_ROOT_UNRESOLVED;
                declared[v]=effect_term_intern(&dense->terms,bound);
                if (dense->terms.status!=XR_XIR_OK) return dense->terms.status;
            }
        }
        if (v<function->parameter_count)
            fixed[v]=(uint8_t)(owner->ordinary->uses.contracts[dense->nodes[index].ordinary].parameters[v].kind!=
                XR_XIR_EFFECT_PARAMETER_VARIABLE);
    }
    if (dense->uses.root && index<dense->uses.count) {
        EffectScalarFlow flow={&dense->terms,function,dense,declared,values,fixed,(uint32_t)count,false,source->declarations};
        XrXirStatus status=effect_scalar_flow(&flow);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!xir_compile_work(dense->terms.remaining,2)) return XR_XIR_BUDGET;
            if (before[i]!=function->instructions[i].type) *changed=true;
        }
    }
    *fixed_output=fixed;return XR_XIR_OK;
}

static XrXirStatus effect_context_dense_expand(EffectContextOwner *owner,
    const XrXirModule *source, uint32_t index, bool *changed) {
    EffectOrdinaryContexts *dense=owner->dense;
    EffectOrdinaryNode identity=dense->nodes[index];
    const XrXirGeneric *generic=source->generics ? &source->generics[identity.declaration] : NULL;
    if (index<dense->base_count && generic && generic->parameter_count) return XR_XIR_OK;
    uint8_t *fixed=NULL;
    XrXirStatus status=effect_context_dense_actuals(owner,source,index,&fixed,changed);
    if (status!=XR_XIR_OK) return status;
    uint32_t count=source->functions[identity.declaration].instruction_count;
    for (uint32_t i=0;i<count;++i) {
        if (!xir_compile_work(dense->terms.remaining,1)) return XR_XIR_BUDGET;
        XrXirInstruction original=source->functions[identity.declaration].instructions[i];
        if (!effect_context_family(original.op)) continue;
        uint32_t ordinary=(uint32_t)owner->ordinary->functions[identity.ordinary].instructions[i].immediate;
        if (ordinary>=owner->ordinary->count) return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *caller=&dense->functions[index];
        XrXirInstruction actual=caller->instructions[i];actual.op=original.op;
        EffectDenseRequest request={.contexts=owner->ordinary,.execution=dense,.selected=ordinary,
            .caller_index=index,.actual_types=&dense->terms.types,.caller=caller,.instruction=&actual,
            .fixed=fixed,.value_count=caller->parameter_count+caller->instruction_count};
        EffectDenseVector vector={0};
        status=effect_dense_build(dense->terms.remaining,&request,&vector);
        uint32_t target=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_context_dense_intern(owner,source,ordinary,&vector,index,&target);
        if (status!=XR_XIR_OK) return status;
        XrXirInstruction *op=(XrXirInstruction *)&dense->functions[index].instructions[i];
        XrXirOp expected=original.op==XR_XIR_CALL_REQUIREMENT || original.op==XR_XIR_CALL_DEFAULT ?
            XR_XIR_CALL : original.op==XR_XIR_INVOKE_DEFAULT ? XR_XIR_INVOKE : original.op;
        if (op->immediate!=target || op->op!=expected) *changed=true;
        op->immediate=target;
        if (original.op==XR_XIR_CALL_REQUIREMENT || original.op==XR_XIR_CALL_DEFAULT) {
            op->op=XR_XIR_CALL;op->targets[0]=op->targets[1]=0;
        } else if (original.op==XR_XIR_INVOKE_DEFAULT) {
            op->op=XR_XIR_INVOKE;op->args[0]=op->args[1]=0;
        }
        op->type_arguments[0]=op->type_arguments[1]=0;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_context_dense_solve(EffectContextOwner *owner,
    const XrXirCompileContext *context, const XrXirModule *source) {
    EffectOrdinaryContexts *dense=owner->dense;
    if (!xir_compile_work(context,(uint64_t)dense->uses.count*
        (sizeof(XrXirFunctionEffectContract)+sizeof(XrXirRootEffects)+5*sizeof(XrXirRootEffectWitness))))
        return XR_XIR_BUDGET;
    effect_context_summary_clear(dense);
    EffectGraph graph={0};EffectOrdinaryView view={0};dense->terms.remaining=context;
    XrXirStatus status=effect_ordinary_view(dense,source,&view);
    if (status==XR_XIR_OK) status=effect_parameters_derive(&view.module,&dense->uses,&graph,context);
    xr_compile_resources_free(graph.flow_rows);
    if (status==XR_XIR_OK) status=effect_ordinary_roots_with_bounds(context,source,dense,owner->declared);
    return status;
}

static XrXirStatus effect_context_dense_stable(const XrXirCompileContext *context,
    EffectContextOwner *owner,const XrXirModule *source);

static XrXirStatus effect_context_owner_build(const XrXirCompileContext *context,
    const XrXirModule *source, EffectRefinementBounds *bounds, EffectContextOwner **output) {
    if (!source || !source->declarations || !source->declarations->functions || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    XrXirModule normalized=*source;
    if (!normalized.types) normalized.types=&effect_empty_types;
    source=&normalized;
    XrXirStatus status=XR_XIR_OK;
    EffectContextOwner *owner=xir_compile_calloc(context,1,sizeof(*owner),&status);
    if (!owner) return status;
    owner->refinement=bounds;
    status=effect_invocation_bounds_new(context,&owner->declared);
    if (status==XR_XIR_OK) status=effect_ordinary_build(context,source,&owner->ordinary);
    if (status==XR_XIR_OK && bounds) {
        owner->ordinary->terms.remaining=context;
        status=effect_refinement_apply(owner->ordinary,source,bounds);
        owner->ordinary->terms.remaining=NULL;
    }
    if (status==XR_XIR_OK) {
        owner->dense=xir_compile_calloc(context,1,sizeof(*owner->dense),&status);
        if (!owner->dense) { effect_context_owner_free(owner);return status; }
        owner->dense->terms.remaining=context;owner->dense->dense=true;
        owner->dense->source_type_count=source->types->count;owner->dense->base_count=source->function_count;
        status=effect_terms_seed(&owner->dense->terms,&owner->ordinary->terms.types);
        /* Once per execution owner, certify the complete cloned declaration
         * domain before any same-pool physical-vector edge is constructed. */
        if (status==XR_XIR_OK)
            status=effect_terms_domain_match(&owner->dense->terms,&owner->ordinary->terms.types);
    }
    EffectOrdinaryContexts *dense=owner->dense;
    if (status==XR_XIR_OK) status=effect_ordinary_capacity(dense,source->function_count);
    for (uint32_t f=0;f<source->function_count && status==XR_XIR_OK;++f) {
        dense->nodes[f]=owner->ordinary->nodes[f];dense->count=f+1;
        dense->nodes[f].physical_types=owner->ordinary->functions[f].parameters;
        /* Rehome the full vector; no classifier storage escapes this owner. */
        uint32_t count=source->functions[f].parameter_count;
        XrXirType *physical=count ? effect_terms_alloc(&dense->terms,count,sizeof(*physical)) : NULL;
        if (count && !physical) { status=dense->terms.status;break; }
        for (uint32_t p=0;p<count;++p) {
            if (!xir_compile_work(context,1)) { status=XR_XIR_BUDGET;break; }
            physical[p]=owner->ordinary->functions[f].parameters[p];
        }
        dense->nodes[f].physical_types=physical;
        if (source->declarations->functions[f].cleanup_owner)
            dense->nodes[f].owner=source->declarations->functions[f].cleanup_owner-1;
        if (status==XR_XIR_OK) status=effect_context_dense_function(owner,source,f);
    }
    bool changed=true, solved=false;
    while (status==XR_XIR_OK && changed) {
        changed=false;uint32_t previous=dense->count,types=dense->terms.types.count;
        dense->terms.remaining=context;
        for (uint32_t f=0;f<dense->count && status==XR_XIR_OK;++f)
            status=effect_context_dense_expand(owner,source,f,&changed);
        if (status==XR_XIR_OK) {
            if (!solved || changed || dense->count!=previous || dense->terms.types.count!=types)
                status=effect_context_dense_solve(owner,context,source);
            else status=effect_context_dense_stable(context,owner,source);
        }
        if (!solved || dense->count!=previous) changed=true;
        solved=true;
    }
    if (status==XR_XIR_OK) status=effect_context_forest_seal(context,source,dense,&owner->forest);
    if (status!=XR_XIR_OK) { effect_context_owner_free(owner);return status; }
    owner->refinement=NULL;
    dense->terms.remaining=NULL;*output=owner;return XR_XIR_OK;
}
