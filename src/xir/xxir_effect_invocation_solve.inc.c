/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_solve.inc.c - Bounded monotone equation initialization and closure
 */
static XrXirStatus effect_invocation_expand(EffectInvocationOwner *owner,
    uint32_t node,bool *changed) {
    if (!changed || node>=owner->count || owner->nodes[node].expanded)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationFlow flow={.owner=owner,.node=node,.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_origins(&flow);
    if (status==XR_XIR_OK) {
        uint32_t body=owner->nodes[node].body;
        if (!owner->local_roots) status=XR_XIR_BAD_STRUCTURE;
        else status=effect_invocation_mask(&flow,
            (owner->local_roots[body].requires_root ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0)|
            (owner->local_roots[body].unresolved ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0),changed);
    }
    for (uint32_t i=0;status==XR_XIR_OK && i<flow.function->instruction_count;++i) {
        flow.instruction=i;status=effect_invocation_instruction(&flow,i,changed);
    }
    if (status==XR_XIR_OK && owner->producer_enabled)
        status=effect_invocation_producer_step(&flow,changed,false);
    xr_compile_resources_free(flow.memory);
    if (status==XR_XIR_OK) {
        EffectInvocationNode *record=&owner->nodes[node];
        uint32_t dependencies=(uint32_t)(((uint64_t)flow.basis.parameters+63)/64);
        uint32_t contexts=(uint32_t)(((uint64_t)flow.function->instruction_count+63)/64);
        uint64_t words=(uint64_t)dependencies*2+contexts;
        if (!xir_compile_work(owner->work,words*sizeof(uint64_t)+
            (uint64_t)record->cause_count*sizeof(*record->causes)+3)) return XR_XIR_BUDGET;
        if (words) memcpy(record->local_cells,record->outputs,(size_t)words*sizeof(uint64_t));
        memcpy(record->local_causes,record->causes,(size_t)record->cause_count*sizeof(*record->causes));
        record->local_mask=record->intrinsic_mask;record->local_deferred=record->deferred;
        record->expanded=true;
    }
    return status;
}

/* Only producer precision is revisited. The immutable declaration seed and
 * local ROOT terminals remain the original expansion; newly certified sites
 * create actual edges on the same equation owner and queue. */
static XrXirStatus effect_invocation_precision(EffectInvocationOwner *owner,
    uint32_t node,bool *changed) {
    EffectInvocationFlow flow={.owner=owner,.node=node,.instruction=UINT32_MAX,.precision=true};
    XrXirStatus status=effect_invocation_origins(&flow);
    for (uint32_t i=0;status==XR_XIR_OK && i<flow.function->instruction_count;++i) {
        XrXirOp op=flow.function->instructions[i].op;
        if (op!=XR_XIR_CALL && op!=XR_XIR_INVOKE && op!=XR_XIR_CALL_DEFAULT &&
            op!=XR_XIR_INVOKE_DEFAULT && op!=XR_XIR_CALL_REQUIREMENT && op!=XR_XIR_CLEANUP_REGISTER &&
            op!=XR_XIR_CALL_INDIRECT && op!=XR_XIR_INVOKE_INDIRECT &&
            op!=XR_XIR_GO && op!=XR_XIR_FUNCTION_REF) continue;
        flow.instruction=i;status=effect_invocation_instruction(&flow,i,changed);
    }
    if (status==XR_XIR_OK) status=effect_invocation_producer_step(&flow,changed,false);
    xr_compile_resources_free(flow.memory);return status;
}

/* The local ROOT declaration seed is expanded once. Finite producer deltas
 * revisit real SSA transfers on this same queue; repeated keys do not copy
 * binding paths. Latent links wake precision without executing a body. */
static XrXirStatus effect_invocations_solve(EffectInvocationOwner *owner) {
    if (!owner || !owner->graph || !owner->work || !owner->module || !owner->count)
        return XR_XIR_BAD_STRUCTURE;
    while (owner->pending) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        uint32_t node=owner->graph->queue[owner->front];
        if (node>=owner->count || !owner->nodes[node].queued) return XR_XIR_BAD_STRUCTURE;
        owner->front=owner->front+1==owner->capacity ? 0 : owner->front+1;
        --owner->pending;owner->nodes[node].queued=false;
        bool changed=false;
        XrXirStatus status=XR_XIR_OK;
        if (!owner->nodes[node].expanded) status=effect_invocation_expand(owner,node,&changed);
        else if (owner->producer_enabled) status=effect_invocation_precision(owner,node,&changed);
        for (uint32_t e=status==XR_XIR_OK ? owner->nodes[node].edge_head : UINT32_MAX;
             e!=UINT32_MAX;e=owner->edges[e].next) {
            if (e>=owner->edge_count) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            EffectInvocationEdge edge=owner->edges[e];
            if (edge.caller!=node || edge.target>=owner->count) return XR_XIR_BAD_STRUCTURE;
            if (!edge.latent) status=effect_invocation_join(owner,node,edge.target,edge.instruction,&changed);
            if (status!=XR_XIR_OK) break;
        }
        if (status!=XR_XIR_OK) return status;
        if (!changed) continue;
        for (uint32_t e=owner->consumers[node];e!=UINT32_MAX;e=owner->edges[e].consumer_next) {
            if (e>=owner->edge_count || owner->edges[e].target!=node) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            status=effect_invocation_enqueue(owner,owner->edges[e].caller);
            if (status!=XR_XIR_OK) return status;
        }
    }
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!owner->nodes[n].expanded || owner->nodes[n].queued) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* This is the existing exhaustive local opcode classifier. Only indirect
 * calls are removed from its advertisement seed and consumed as authenticated
 * invocation equations instead. Requirement UNKNOWN remains intrinsic. */
static XrXirStatus effect_invocation_intrinsics(EffectInvocationOwner *owner) {
    uint32_t count=owner->module->function_count;
    if (owner->local_roots || owner->local_witnesses ||
        (uint64_t)count>SIZE_MAX/sizeof(*owner->local_roots) ||
        (uint64_t)count*2>SIZE_MAX/sizeof(*owner->local_witnesses)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    owner->local_roots=xir_compile_calloc(owner->work,count,sizeof(*owner->local_roots),&status);
    if (!owner->local_roots) return status;
    owner->local_witnesses=xir_compile_calloc(owner->work,(size_t)count*2,
        sizeof(*owner->local_witnesses),&status);
    if (!owner->local_witnesses) return status;
    XrXirEffects local={.count=count,.root=owner->local_roots,
        .root_witnesses=owner->local_witnesses,.unresolved_witnesses=owner->local_witnesses+count};
    const XrXirDeclarations *declarations=owner->module->declarations;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        const XrXirFunction *function=&owner->module->functions[f];
        if (declarations) {
            if (!declarations->functions || !declarations->modules ||
                declarations->functions[f].module>=declarations->module_count)
                return XR_XIR_BAD_STRUCTURE;
            if (declarations->modules[declarations->functions[f].module].initializer==f) {
                if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
                local.root[f].requires_root=true;
                local.root_witnesses[f]=(XrXirRootEffectWitness){.cause=XR_XIR_ROOT_CAUSE_INITIALIZER,
                    .instruction=UINT32_MAX,.callee=UINT32_MAX,.slot=UINT32_MAX};
            }
        }
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            XrXirOp op=function->instructions[i].op;
            if (op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT) continue;
            status=effect_root_seed(owner->module,&local,f,i,owner->work);
            if (status!=XR_XIR_OK) return status;
            if (op==XR_XIR_CALL_REQUIREMENT) {
                if (!xir_compile_work(owner->work,1+sizeof(XrXirRootEffectWitness))) return XR_XIR_BUDGET;
                local.root[f].unresolved=false;
                local.unresolved_witnesses[f]=(XrXirRootEffectWitness){0};
            }
        }
    }
    return XR_XIR_OK;
}

/* Root formal seeds do not certify an actual binding. Scoped Cell formals
 * are explicit dependencies, and REF callable formals are deferred symbols.
 * Every other fixed callable retains its complete declared advertisement. */
static XrXirStatus effect_invocation_root_input(EffectInvocationOwner *owner,
    uint32_t function,uint64_t **output) {
    if (!output || *output || function>=owner->module->function_count || !owner->effects->contracts)
        return XR_XIR_BAD_STRUCTURE;
    if (!owner->root_spaces || owner->root_spaces[function]>=owner->module->function_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(owner,owner->root_spaces[function],&basis);
    if (status!=XR_XIR_OK) return status;
    const XrXirFunction *body=&owner->module->functions[function];
    uint64_t words=(uint64_t)body->parameter_count*basis.row_words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    uint64_t *input=words ? xir_compile_calloc(owner->work,(size_t)words,sizeof(*input),&status) : NULL;
    if (words && !input) return status;
    if (owner->effects->contracts[function].parameter_count!=body->parameter_count ||
        (body->parameter_count && (!body->parameters || !owner->effects->contracts[function].parameters))) {
        xr_compile_resources_free(input);return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t p=0;p<body->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->work,3)) { status=XR_XIR_BUDGET;break; }
        uint64_t *row=input+(size_t)p*basis.row_words;
        if (xr_xir_type_is_cell(owner->module->types,body->parameters[p])) {
            uint32_t known=basis.parameters+2;
            row[basis.fn_words+p/64]|=UINT64_C(1)<<(p%64);
            row[basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
            continue;
        }
        const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,body->parameters[p]);
        if (!signature) continue;
        bool ref=false;
        for (uint32_t a=0;a<signature->parameter_count;++a) {
            if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
            if (!xr_xir_callable_parameter_storage_valid(owner->module->types,&signature->parameters[a])) {
                status=XR_XIR_BAD_TYPE;break;
            }
            if (signature->parameters[a].mode==XR_PARAM_REF) ref=true;
        }
        if (status!=XR_XIR_OK) break;
        uint32_t mask=0;
        status=effect_invocation_bounds_mask(owner->work,owner->declared,function,p,&mask);
        if (status!=XR_XIR_OK) break;
        const XrXirEffectParameter *parameter=&owner->effects->contracts[function].parameters[p];
        if (mask && ((ref && (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)) ||
            parameter->kind==XR_XIR_EFFECT_PARAMETER_VARIABLE)) {
            uint32_t bit=owner->site_count+p;
            row[bit/64]|=UINT64_C(1)<<(bit%64);
            if (ref && (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)) {
                uint32_t opaque=owner->site_count+basis.parameters;
                row[opaque/64]|=UINT64_C(1)<<(opaque%64);
                row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
            }
        } else {
            uint32_t opaque=owner->site_count+basis.parameters;
            row[opaque/64]|=UINT64_C(1)<<(opaque%64);
            if (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)
                row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
            if (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)
                row[(opaque+2)/64]|=UINT64_C(1)<<((opaque+2)%64);
        }
    }
    if (status!=XR_XIR_OK) { xr_compile_resources_free(input);return status; }
    *output=input;return XR_XIR_OK;
}

/* A complete physical-formal scan proves whether a root has any callable
 * or Cell dependency column. Roots with none share one fixed empty origin
 * namespace; their different body IDs and complete actual input vectors still
 * remain distinct keys. No body, domain or permission verification is skipped. */
static XrXirStatus effect_invocation_root_spaces(EffectInvocationOwner *owner) {
    uint32_t count=owner->module->function_count;
    if ((uint64_t)count>SIZE_MAX/sizeof(*owner->root_spaces)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    owner->root_spaces=xir_compile_alloc(owner->work,(size_t)count*sizeof(*owner->root_spaces),&status);
    if (!owner->root_spaces) return status;
    uint32_t shared=UINT32_MAX;
    for (uint32_t f=0;f<count;++f) {
        const XrXirFunction *function=&owner->module->functions[f];bool tracked=false;
        for (uint32_t p=0;p<function->parameter_count;++p) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            XrXirType type=function->parameters[p];
            if (xr_xir_callable_signature(owner->module->types,type) || xr_xir_type_is_cell(owner->module->types,type))
                tracked=true;
        }
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!tracked && shared==UINT32_MAX) shared=f;
        owner->root_spaces[f]=tracked?f:shared;
    }
    return XR_XIR_OK;
}

/* Private creation runs inside the existing formula owner after full source,
 * body, domain, physical-role and construction verification. The structural
 * limit product bounds actual equation IDs; the fixed bit basis proves finite
 * state independently of that practical admission limit. */
static XrXirStatus effect_invocations_create(const XrXirCompileContext *work,
    const XrXirModule *module,XrXirEffects *effects,EffectGraph *graph,
    const EffectInvocationDeclaredBounds *declared,
    EffectInvocationOwner **output) {
    if (!output || *output || !module || !module->functions || !graph || !effects ||
        !xir_effects_context_matches(work,effects,module->function_count) || !effects->contracts ||
        !declared || declared->resources!=work->resources || declared->count!=module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        uint64_t values=(uint64_t)module->functions[f].parameter_count+module->functions[f].instruction_count;
        if (values!=declared->functions[f].values ||
            (values && !declared->functions[f].bounds)) return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t maximum=(uint64_t)work->limits.functions*work->limits.instructions;
    /* Wide default structural allowances need not fit an equation ordinal.
     * Clamp only representability; actual storage/work still use the original
     * common ledger, and the fixed basis independently proves finiteness. */
    if (maximum>UINT32_MAX) maximum=UINT32_MAX;
    if (!maximum || maximum<module->function_count) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationOwner *owner=xir_compile_calloc(work,1,sizeof(*owner),&status);
    if (!owner) return status;
    *owner=(EffectInvocationOwner){.work=work,.module=module,.effects=effects,
        .graph=graph,.declared=declared,.maximum=(uint32_t)maximum};
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f) {
        bool supported=false;status=effect_invocation_producer_return(owner,f,&supported);
        owner->producer_enabled|=supported;
    }
    if (status==XR_XIR_OK) status=effect_invocation_root_spaces(owner);
    if (status==XR_XIR_OK) status=effect_invocation_sites(owner);
    if (status==XR_XIR_OK) status=effect_invocation_intrinsics(owner);
    if (status==XR_XIR_OK) {
        if ((uint64_t)module->function_count>SIZE_MAX/sizeof(*owner->roots)) status=XR_XIR_BUDGET;
        else owner->roots=xir_compile_alloc(work,(size_t)module->function_count*sizeof(*owner->roots),&status);
        if (status==XR_XIR_OK && !owner->roots) status=XR_XIR_OUT_OF_MEMORY;
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f) {
        uint64_t *input=NULL;
        status=effect_invocation_root_input(owner,f,&input);
        uint32_t node=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_invocation_intern(owner,owner->root_spaces[f],f,input,&node);
        xr_compile_resources_free(input);
        if (status==XR_XIR_OK) {
            if (!xir_compile_work(work,1)) status=XR_XIR_BUDGET;
            else owner->roots[f]=node;
        }
    }
    if (status!=XR_XIR_OK) { effect_invocation_free(owner);return status; }
    *output=owner;return XR_XIR_OK;
}
