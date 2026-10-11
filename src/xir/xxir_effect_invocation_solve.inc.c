/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_solve.inc.c - Bounded monotone equation initialization and closure
 */
/* Precision retains the original independent opcode frontier. This answers
 * only which complete decoded row enters the unchanged instruction handler. */
static bool effect_invocation_dispatch_precision(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_CALL_DEFAULT ||
        op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_CALL_REQUIREMENT || op==XR_XIR_CLEANUP_REGISTER ||
        op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT ||
        op==XR_XIR_GO || op==XR_XIR_FUNCTION_REF || effect_invocation_cell_terminal(op);
}

/* Raw and mixed callbacks retain their original selected-row reader and
 * costs. No custom reader is treated as a canonical owning representation. */
static XrXirStatus effect_invocation_dispatch_row(EffectInvocationFlow *flow,
    uint32_t instruction,bool *changed) {
    if (!flow->precision) {
        flow->instruction=instruction;return effect_invocation_instruction(flow,instruction,changed);
    }
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_invocation_flow_instruction(flow,instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (!effect_invocation_dispatch_precision(op->op)) return XR_XIR_OK;
    if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
    flow->instruction=instruction;
    return effect_invocation_instruction_decoded(flow,instruction,op,changed);
}

static XrXirStatus effect_invocation_dispatch_scalar(EffectInvocationFlow *flow,bool *changed) {
    EffectInvocationOwner *owner=flow->owner;
    for (uint32_t r=0;r<flow->physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        if (record.value<flow->function->parameter_count || record.value>=flow->values)
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status=effect_invocation_dispatch_row(flow,
            record.value-flow->function->parameter_count,changed);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A canonical constant run has ten identical physical fields. The complete
 * stack value remains local to this run; every selected ordinal still enters
 * the original handler with its own current coordinate and semantic charges. */
static XrXirStatus effect_invocation_dispatch_constant(EffectInvocationFlow *flow,
    const EffectBodyRunRef *ref,uint32_t *record_index,bool *changed) {
    if (ref->mask || *record_index>=flow->physical->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->owner->work,2*sizeof(uint32_t)+6)) return XR_XIR_BUDGET;
    uint32_t parameters=flow->function->parameter_count;
    uint32_t first=flow->physical->records[*record_index].value;
    if (first<parameters || first>=flow->values || first-parameters<ref->first ||
        first-parameters>=ref->first+ref->count) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction op;
    XrXirStatus status=effect_body_instruction_ref_value(&flow->body.read,ref,first-parameters,&op);
    if (status!=XR_XIR_OK) return status;
    while (*record_index<flow->physical->count &&
        flow->physical->records[*record_index].value-parameters<ref->first+ref->count) {
        if (!xir_compile_work(flow->owner->work,3+sizeof(*record_index))) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[(*record_index)++];
        if (record.value<parameters || record.value>=flow->values ||
            record.value-parameters<ref->first) return XR_XIR_BAD_STRUCTURE;
        if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        uint32_t i=record.value-parameters;
        if (!flow->precision && !xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
        if (flow->precision) {
            if (!effect_invocation_dispatch_precision(op.op)) continue;
            if (!xir_compile_work(flow->owner->work,1)) return XR_XIR_BUDGET;
        }
        flow->instruction=i;status=effect_invocation_instruction_decoded(flow,i,&op,changed);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* The first selected row used the complete scalar path. Only a second real
 * invocation starts this fresh owning walk, so unused and singleton requests
 * never initialize an unused 40-byte row or scan the rest of their body. */
static XrXirStatus effect_invocation_dispatch_range(EffectInvocationFlow *flow,
    uint32_t start,bool *changed) {
    EffectInvocationOwner *owner=flow->owner;
    const EffectBodyFunction *body=&owner->material->functions[owner->nodes[flow->node].body];
    if (start>=flow->physical->count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor={0,0,0,UINT32_MAX};uint32_t r=start;
    if (!xir_compile_work(owner->work,8+sizeof(cursor)+sizeof(r))) return XR_XIR_BUDGET;
    for (uint32_t run=0;run<body->run_count;++run) {
        EffectBodyRunRef ref;XrXirStatus status=effect_body_range_ref(&flow->body.read,body,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!ref.mask) {
            while (r<flow->physical->count &&
                flow->physical->records[r].value-body->parameter_count<ref.first+ref.count &&
                !(flow->physical->records[r].mask&EFFECT_PHYSICAL_INVOCATION)) {
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                uint32_t value=flow->physical->records[r++].value;
                if (value<body->parameter_count || value>=flow->values ||
                    value-body->parameter_count<ref.first) return XR_XIR_BAD_STRUCTURE;
            }
            if (r<flow->physical->count &&
                flow->physical->records[r].value-body->parameter_count<ref.first+ref.count) {
                status=effect_invocation_dispatch_constant(flow,&ref,&r,changed);
                if (status!=XR_XIR_OK) return status;
            }
            continue;
        }
        while (r<flow->physical->count &&
            flow->physical->records[r].value-body->parameter_count<ref.first+ref.count) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=flow->physical->records[r++];
            if (record.value<body->parameter_count || record.value>=flow->values ||
                record.value-body->parameter_count<ref.first) return XR_XIR_BAD_STRUCTURE;
            if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
            uint32_t i=record.value-body->parameter_count;
            if (!flow->precision && !xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            XrXirInstruction op;
            status=effect_body_instruction_ref_value(&flow->body.read,&ref,i,&op);
            if (status!=XR_XIR_OK) return status;
            if (flow->precision) {
                if (!effect_invocation_dispatch_precision(op.op)) continue;
                if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            }
            flow->instruction=i;status=effect_invocation_instruction_decoded(flow,i,&op,changed);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return r==flow->physical->count && cursor.first==body->instruction_count &&
        cursor.offset==body->stream_words?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* Canonical ownership selects a representation reader, not an effect fact.
 * Raw or mixed requests keep their original callbacks and all terminal proof
 * gates independently replay the complete physical input and stored result. */
static XrXirStatus effect_invocation_dispatch(EffectInvocationFlow *flow,bool *changed) {
    if (!flow || !flow->owner || !flow->function || !flow->physical || !changed ||
        flow->node>=flow->owner->count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationOwner *owner=flow->owner;
    if (flow->body.instruction!=effect_body_owned_instruction ||
        flow->body.head!=effect_body_owned_head || flow->body.type!=effect_body_owned_type ||
        flow->body.shape!=effect_body_owned_shape ||
        flow->body.read.consume!=effect_body_compile_consume)
        return effect_invocation_dispatch_scalar(flow,changed);
    const EffectBodyMaterial *material=flow->body.owner;
    uint32_t f=owner->nodes[flow->node].body;
    if (material!=owner->material || !xir_compile_context_valid(owner->work) ||
        !effect_body_material_shape(material) || material->resources!=owner->work->resources ||
        f>=effect_body_count(material) || flow->body.read.owner!=owner->work ||
        flow->body.domain.types!=material->domain.types ||
        flow->body.domain.declarations!=material->domain.declarations ||
        flow->body.domain.generics!=material->domain.generics ||
        flow->body.domain.defaults!=material->domain.defaults ||
        flow->body.domain.function_count!=effect_body_count(material) || !material->physical ||
        material->physical->count!=effect_body_count(material) || !material->physical->functions ||
        owner->physical!=material->physical ||
        flow->physical!=&material->physical->functions[f]) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&material->functions[f];
    if (body->parameter_count!=flow->function->parameter_count ||
        body->instruction_count!=flow->function->instruction_count ||
        material->physical->resources!=owner->work->resources ||
        flow->physical->parameters!=body->parameter_count ||
        flow->physical->instructions!=body->instruction_count ||
        flow->physical->values!=(uint64_t)body->parameter_count+body->instruction_count ||
        flow->values!=flow->physical->values || flow->physical->count>flow->physical->capacity ||
        (flow->physical->count && !flow->physical->records)) return XR_XIR_BAD_STRUCTURE;
    if (flow->physical->count<=1 || body->run_count<=1)
        return effect_invocation_dispatch_scalar(flow,changed);
    bool selected=false;
    if (!xir_compile_work(owner->work,sizeof(selected))) return XR_XIR_BUDGET;
    for (uint32_t r=0;r<flow->physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        if (record.value<body->parameter_count || record.value>=flow->values) return XR_XIR_BAD_STRUCTURE;
        if (selected) return effect_invocation_dispatch_range(flow,r,changed);
        XrXirStatus status=effect_invocation_dispatch_row(flow,record.value-body->parameter_count,changed);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(owner->work,sizeof(selected))) return XR_XIR_BUDGET;
        selected=true;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_expand(EffectInvocationOwner *owner,
    uint32_t node,bool *changed) {
    if (!changed || node>=owner->count || owner->nodes[node].expanded)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationFlow flow={.owner=owner,.node=node,.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_origins(&flow);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    if (status==XR_XIR_OK) {
        uint32_t body=owner->nodes[node].body;
        if (!owner->local_roots) status=XR_XIR_BAD_STRUCTURE;
        else status=effect_invocation_mask(&flow,
            (owner->local_roots[body].requires_root ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0)|
            (owner->local_roots[body].unresolved ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0),changed);
    }
    if (status==XR_XIR_OK) status=effect_invocation_dispatch(&flow,changed);
    if (status==XR_XIR_OK && owner->producer_enabled)
        status=effect_invocation_producer_step(&flow,changed,false);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    xr_compile_resources_free(flow.memory);
    if (status==XR_XIR_OK) {
        EffectInvocationNode *record=&owner->nodes[node];
        uint32_t dependencies=(uint32_t)(((uint64_t)flow.basis.parameters+63)/64);
        uint32_t contexts=(uint32_t)(((uint64_t)flow.function->instruction_count+63)/64);
        uint64_t words=(uint64_t)dependencies*2+contexts;
        if (!xir_compile_work(owner->work,8)) return XR_XIR_BUDGET;
        if (!effect_invocation_node_shape(record,flow.basis.parameters,
            flow.function->instruction_count)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,words*sizeof(uint64_t)+
            (uint64_t)record->cause_count*sizeof(*record->causes)+3)) return XR_XIR_BUDGET;
        if (words) memcpy(record->local_cells,record->cells,(size_t)words*sizeof(uint64_t));
        memcpy(effect_invocation_local_causes(record),record->causes,(size_t)record->cause_count*sizeof(*record->causes));
        record->local_mask=record->intrinsic_mask;record->local_deferred=record->deferred;
        record->expanded=true;
    }
    return status;
}

/* Actual producer and Class precision revisit the same equation queue.
 * Immutable declaration seeds are never expanded again; delayed Cell
 * terminals add only their genuine local instruction evidence. */
static XrXirStatus effect_invocation_precision(EffectInvocationOwner *owner,
    uint32_t node,bool *changed) {
    EffectInvocationFlow flow={.owner=owner,.node=node,.instruction=UINT32_MAX,.precision=true};
    XrXirStatus status=effect_invocation_origins(&flow);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    if (status==XR_XIR_OK) status=effect_invocation_dispatch(&flow,changed);
    if (status==XR_XIR_OK && owner->producer_enabled) status=effect_invocation_producer_step(&flow,changed,false);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    xr_compile_resources_free(flow.memory);return status;
}

/* The local ROOT declaration seed is expanded once. Finite producer deltas
 * revisit real SSA transfers on this same queue; repeated keys do not copy
 * binding paths. Latent links wake precision without executing a body. */
static XrXirStatus effect_invocations_solve(EffectInvocationOwner *owner) {
    if (!owner || !owner->graph || !owner->work || !effect_body_material_shape(owner->material) || !owner->count)
        return XR_XIR_BAD_STRUCTURE;
    bool residual=true;
    while (owner->pending || residual) {
        if (!owner->pending) {
            XrXirStatus fill=effect_invocation_class_residual(owner,&residual);
            if (fill!=XR_XIR_OK) return fill;
            if (!owner->pending) break;
        }
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        uint32_t node=owner->graph->queue[owner->front];
        if (node>=owner->count || !owner->nodes[node].queued) return XR_XIR_BAD_STRUCTURE;
        owner->front=owner->front+1==owner->capacity ? 0 : owner->front+1;
        --owner->pending;owner->nodes[node].queued=false;
        bool changed=false;
        XrXirStatus status=XR_XIR_OK;
        if (!owner->nodes[node].expanded) status=effect_invocation_expand(owner,node,&changed);
        else if (owner->producer_enabled || owner->class_enabled) status=effect_invocation_precision(owner,node,&changed);
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
    return effect_invocation_coverage_solve(owner);
}

/* This is the existing exhaustive local opcode classifier. Only indirect
 * calls are removed from its advertisement seed and consumed as authenticated
 * invocation equations instead. Requirement UNKNOWN remains intrinsic. */
static XrXirStatus effect_invocation_intrinsics(EffectInvocationOwner *owner) {
    if (!owner || !effect_body_material_shape(owner->material) || owner->local_roots ||
        owner->local_witnesses || !owner->material->local_roots || !owner->material->local_witnesses)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=owner->body.domain.function_count;
    if ((uint64_t)count>SIZE_MAX/sizeof(*owner->local_roots) ||
        (uint64_t)count*2>SIZE_MAX/sizeof(*owner->local_witnesses)) return XR_XIR_BUDGET;
    /* The generation owns these immutable seeds before any equation is built.
     * The equation owner dies before its transferred material on every path. */
    if (!xir_compile_work(owner->work,sizeof(owner->local_roots)+sizeof(owner->local_witnesses)))
        return XR_XIR_BUDGET;
    owner->local_roots=owner->material->local_roots;
    owner->local_witnesses=owner->material->local_witnesses;
    return XR_XIR_OK;
}

/* Root formal seeds do not certify an actual binding. Scoped Cell formals
 * are explicit dependencies, and REF callable formals are deferred symbols.
 * Every other fixed callable retains its complete declared advertisement. */
static XrXirStatus effect_invocation_root_input(EffectInvocationOwner *owner,
    uint32_t function,uint64_t **output) {
    if (!output || *output || function>=owner->body.domain.function_count || !owner->effects->contracts)
        return XR_XIR_BAD_STRUCTURE;
    if (!owner->root_spaces || owner->root_spaces[function]>=owner->body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(owner,owner->root_spaces[function],&basis);
    if (status!=XR_XIR_OK) return status;
    EffectBodyFunctionHead head={0};status=effect_invocation_head(owner,function,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionHead *body=&head;
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
        if (xr_xir_type_is_cell(owner->body.domain.types,body->parameters[p])) {
            uint32_t known=basis.parameters+2;
            row[basis.fn_words+p/64]|=UINT64_C(1)<<(p%64);
            row[basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
            continue;
        }
        if (xr_xir_type_is_class(owner->body.domain.types,body->parameters[p])) {
            uint32_t unknown=basis.parameters+1,known=unknown+1;
            row[basis.fn_words+unknown/64]|=UINT64_C(1)<<(unknown%64);
            row[basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
            continue;
        }
        const XrXirTypeNode *signature=xr_xir_callable_signature(owner->body.domain.types,body->parameters[p]);
        if (!signature) continue;
        bool ref=false;
        for (uint32_t a=0;a<signature->parameter_count;++a) {
            if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
            if (!xr_xir_callable_parameter_storage_valid(owner->body.domain.types,&signature->parameters[a])) {
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
    uint32_t count=owner->body.domain.function_count;
    if ((uint64_t)count>SIZE_MAX/sizeof(*owner->root_spaces)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    owner->root_spaces=xir_compile_alloc(owner->work,(size_t)count*sizeof(*owner->root_spaces),&status);
    if (!owner->root_spaces) return status;
    uint32_t shared=UINT32_MAX;
    for (uint32_t f=0;f<count;++f) {
        EffectBodyFunctionHead head={0};status=effect_invocation_head(owner,f,&head);
        if (status!=XR_XIR_OK) return status;
        const EffectBodyFunctionHead *function=&head;bool tracked=false;
        for (uint32_t p=0;p<function->parameter_count;++p) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            XrXirType type=function->parameters[p];
            if (xr_xir_callable_signature(owner->body.domain.types,type) || xr_xir_type_is_cell(owner->body.domain.types,type) ||
                xr_xir_type_is_class(owner->body.domain.types,type))
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
    uint64_t maximum=(uint64_t)work->limits.functions*work->limits.instructions;
    /* Wide default structural allowances need not fit an equation ordinal.
     * Clamp only representability; actual storage/work still use the original
     * common ledger, and the fixed basis independently proves finiteness. */
    if (maximum>UINT32_MAX) maximum=UINT32_MAX;
    if (!maximum || maximum<module->function_count) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    if (!graph->material || (graph->material!=effects->generation && !graph->physical_owned))
        status=effect_graph_physical_refresh(work,module,graph,effects->refinement);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_material_shape(graph->material) || graph->material->resources!=work->resources ||
        graph->physical!=graph->material->physical || !graph->material->cells) return XR_XIR_BAD_STRUCTURE;
    if (!declared->functions || !graph->material->declared) return XR_XIR_BAD_STRUCTURE;
    if (declared!=graph->material->declared) {
        EffectInvocationDeclaredBounds *copy=NULL;
        status=effect_invocation_bounds_copy_physical(work,declared,graph->physical,&copy);
        if (status==XR_XIR_OK && !xir_compile_work(work,2)) status=XR_XIR_BUDGET;
        if (status!=XR_XIR_OK) { effect_invocation_bounds_free(copy);return status; }
        effect_invocation_bounds_free(graph->material->declared);graph->material->declared=copy;
    }
    declared=graph->material->declared;
    EffectInvocationOwner *owner=xir_compile_calloc(work,1,sizeof(*owner),&status);
    if (!owner) return status;
    *owner=(EffectInvocationOwner){.work=work,.module=module,.effects=effects,
        .graph=graph,.declared=declared,.maximum=(uint32_t)maximum,
        .material=graph->material,.physical=graph->material->physical,.cells=graph->material->cells,
        .body=effect_body_owned_request(work,graph->material)};
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f) {
        bool supported=false;status=effect_invocation_producer_return(owner,f,&supported);
        owner->producer_enabled|=supported;
    }

    if (status==XR_XIR_OK) {
        for (uint32_t f=0;f<owner->physical->count;++f) {
            if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
            EffectBodyFunctionHead head={0};status=effect_invocation_head(owner,f,&head);
            if (status!=XR_XIR_OK) break;
            owner->class_enabled|=owner->physical->functions[f].classes!=0 ||
                xr_xir_type_is_class(owner->body.domain.types,head.result);
        }
    }
    if (status==XR_XIR_OK && owner->class_enabled) {
        EffectBodyRequest body=effect_invocation_body(owner);
        status=effect_invocation_class_cells_match(owner,&body);
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
