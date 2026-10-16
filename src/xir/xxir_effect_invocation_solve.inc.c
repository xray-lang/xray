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
    if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
    flow->instruction=instruction;
    return effect_invocation_instruction_decoded(flow,instruction,op,changed);
}

static XrXirStatus effect_invocation_dispatch_scalar(EffectInvocationFlow *flow,bool *changed) {
    const EffectInvocationRead *owner=flow->read;
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
    const EffectBodyRun *ref,uint32_t *record_index,bool *changed) {
    if (ref->mask || *record_index>=flow->physical->count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->read->work,2*sizeof(uint32_t)+6)) return XR_XIR_BUDGET;
    uint32_t parameters=flow->function->parameter_count;
    uint32_t first=flow->physical->records[*record_index].value;
    if (first<parameters || first>=flow->values || first-parameters<ref->first ||
        first-parameters>=ref->first+ref->count) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction op;
    XrXirStatus status=effect_body_instruction_ref_value(&flow->body.read,ref,first-parameters,&op);
    if (status!=XR_XIR_OK) return status;
    while (*record_index<flow->physical->count &&
        flow->physical->records[*record_index].value-parameters<ref->first+ref->count) {
        if (!xir_compile_work(flow->read->work,3+sizeof(*record_index))) return XR_XIR_BUDGET;
        EffectPhysicalRecord record=flow->physical->records[(*record_index)++];
        if (record.value<parameters || record.value>=flow->values ||
            record.value-parameters<ref->first) return XR_XIR_BAD_STRUCTURE;
        if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        uint32_t i=record.value-parameters;
        if (!flow->precision && !xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
        if (flow->precision) {
            if (!effect_invocation_dispatch_precision(op.op)) continue;
            if (!xir_compile_work(flow->read->work,1)) return XR_XIR_BUDGET;
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
    const EffectInvocationRead *owner=flow->read;
    const EffectBodyFunction *body=&owner->final->material->functions[owner->final->nodes[flow->node].body];
    if (start>=flow->physical->count) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};uint32_t r=start;
    if (!xir_compile_work(owner->work,8+sizeof(cursor)+sizeof(r))) return XR_XIR_BUDGET;
    for (uint32_t run=0;run<body->run_count;++run) {
        const EffectBodyRun *ref;XrXirStatus status=effect_body_range_ref(&flow->body.read,body,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        if (!ref->mask) {
            while (r<flow->physical->count &&
                flow->physical->records[r].value-body->parameter_count<ref->first+ref->count &&
                !(flow->physical->records[r].mask&EFFECT_PHYSICAL_INVOCATION)) {
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                uint32_t value=flow->physical->records[r++].value;
                if (value<body->parameter_count || value>=flow->values ||
                    value-body->parameter_count<ref->first) return XR_XIR_BAD_STRUCTURE;
            }
            if (r<flow->physical->count &&
                flow->physical->records[r].value-body->parameter_count<ref->first+ref->count) {
                status=effect_invocation_dispatch_constant(flow,ref,&r,changed);
                if (status!=XR_XIR_OK) return status;
            }
            continue;
        }
        while (r<flow->physical->count &&
            flow->physical->records[r].value-body->parameter_count<ref->first+ref->count) {
            if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
            EffectPhysicalRecord record=flow->physical->records[r++];
            if (record.value<body->parameter_count || record.value>=flow->values ||
                record.value-body->parameter_count<ref->first) return XR_XIR_BAD_STRUCTURE;
            if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
            uint32_t i=record.value-body->parameter_count;
            if (!flow->precision && !xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
            XrXirInstruction op;
            status=effect_body_instruction_ref_value(&flow->body.read,ref,i,&op);
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
        cursor.run==body->run_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* Canonical ownership selects a representation reader, not an effect fact.
 * Raw or mixed requests keep their original callbacks and all terminal proof
 * gates independently replay the complete physical input and stored result. */
static XrXirStatus effect_invocation_dispatch(EffectInvocationFlow *flow,bool *changed) {
    if (!flow || !flow->owner || !flow->function || !flow->physical || !changed ||
        flow->node>=flow->read->final->count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationRead *owner=flow->read;
    if (flow->body.instruction!=effect_body_owned_instruction ||
        flow->body.head!=effect_body_owned_head || flow->body.type!=effect_body_owned_type ||
        flow->body.shape!=effect_body_owned_shape ||
        flow->body.read.consume!=effect_body_compile_consume)
        return effect_invocation_dispatch_scalar(flow,changed);
    const EffectBodyMaterial *material=flow->body.owner;
    uint32_t f=owner->final->nodes[flow->node].body;
    if (material!=owner->final->material || !xir_compile_context_valid(owner->work) ||
        !effect_body_material_shape(material) || material->resources!=owner->work->resources ||
        f>=effect_body_count(material) || flow->body.read.owner!=owner->work ||
        flow->body.domain.types!=material->domain.types ||
        flow->body.domain.declarations!=material->domain.declarations ||
        flow->body.domain.generics!=material->domain.generics ||
        flow->body.domain.defaults!=material->domain.defaults ||
        flow->body.domain.function_count!=effect_body_count(material) || !material->physical ||
        material->physical->count!=effect_body_count(material) || !material->physical->functions ||
        owner->final->physical!=material->physical ||
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

static XrXirStatus effect_invocation_expand(EffectInvocationWorkspace *owner,
    uint32_t node,bool *changed) {
    if (!changed || node>=owner->final.count || !owner->states || owner->states[node].expanded)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationFlow flow={.owner=owner,.read=&owner->read,.node=node,.instruction=UINT32_MAX};
    XrXirStatus status=effect_invocation_origins(&flow);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    if (status==XR_XIR_OK) {
        uint32_t body=owner->final.nodes[node].body;
        if (!owner->final.local_roots) status=XR_XIR_BAD_STRUCTURE;
        else status=effect_invocation_mask(&flow,
            (owner->final.local_roots[body].requires_root ? XR_XIR_CALLABLE_ROOT_REQUIRED : 0)|
            (owner->final.local_roots[body].unresolved ? XR_XIR_CALLABLE_ROOT_UNRESOLVED : 0),changed);
    }
    if (status==XR_XIR_OK) status=effect_invocation_dispatch(&flow,changed);
    if (status==XR_XIR_OK && owner->read.producer_enabled)
        status=effect_invocation_producer_step(&flow,changed,false);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    effect_invocation_flow_finish(&flow);
    if (status==XR_XIR_OK) {
        EffectInvocationNode *record=&owner->final.nodes[node];
        uint32_t dependencies=(uint32_t)(((uint64_t)flow.basis.parameters+63)/64);
        uint32_t contexts=(uint32_t)(((uint64_t)flow.function->instruction_count+63)/64);
        uint64_t words=(uint64_t)dependencies*2+contexts;
        if (!xir_compile_work(owner->read.work,9)) return XR_XIR_BUDGET;
        if (!effect_invocation_node_shape(record,flow.basis.parameters,
            flow.function->instruction_count)) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->read.work,words*sizeof(uint64_t)+
            (uint64_t)record->cause_count*sizeof(*record->causes)+3)) return XR_XIR_BUDGET;
        if (words) memcpy(record->local_cells,record->cells,(size_t)words*sizeof(uint64_t));
        memcpy(effect_invocation_local_causes(record),record->causes,(size_t)record->cause_count*sizeof(*record->causes));
        record->local_mask=record->intrinsic_mask;record->local_deferred=record->deferred;
        owner->states[node].expanded=true;
    }
    return status;
}

/* Actual producer and Class precision revisit the same equation queue.
 * Immutable declaration seeds are never expanded again; delayed Cell
 * terminals add only their genuine local instruction evidence. */
static XrXirStatus effect_invocation_precision(EffectInvocationWorkspace *owner,
    uint32_t node,bool *changed) {
    EffectInvocationFlow flow={.owner=owner,.read=&owner->read,.node=node,.instruction=UINT32_MAX,.precision=true};
    XrXirStatus status=effect_invocation_origins(&flow);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    if (status==XR_XIR_OK) status=effect_invocation_dispatch(&flow,changed);
    if (status==XR_XIR_OK && owner->read.producer_enabled) status=effect_invocation_producer_step(&flow,changed,false);
    if (status==XR_XIR_OK) status=effect_invocation_class_step(&flow,changed);
    effect_invocation_flow_finish(&flow);return status;
}

/* The local ROOT declaration seed is expanded once. Finite producer deltas
 * revisit real SSA transfers on this same queue; repeated keys do not copy
 * binding paths. Latent links wake precision without executing a body. */
static XrXirStatus effect_invocations_solve(EffectInvocationWorkspace *owner) {
    if (!owner || !owner->graph || !owner->read.work || !owner->states ||
        !effect_body_material_shape(owner->final.material) || !owner->final.count)
        return XR_XIR_BAD_STRUCTURE;
    bool residual=true;
    while (owner->pending || residual) {
        if (!owner->pending) {
            XrXirStatus fill=effect_invocation_class_residual(owner,&residual);
            if (fill!=XR_XIR_OK) return fill;
            if (!owner->pending) break;
        }
        if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
        uint32_t node=owner->graph->queue[owner->front];
        if (node>=owner->final.count || !owner->states[node].queued) return XR_XIR_BAD_STRUCTURE;
        owner->front=owner->front+1==owner->final.capacity ? 0 : owner->front+1;
        --owner->pending;owner->states[node].queued=false;
        bool changed=false;
        XrXirStatus status=XR_XIR_OK;
        if (!owner->states[node].expanded) status=effect_invocation_expand(owner,node,&changed);
        else if (owner->read.producer_enabled || owner->read.class_enabled) status=effect_invocation_precision(owner,node,&changed);
        for (uint32_t e=status==XR_XIR_OK ? owner->final.nodes[node].edge_head : UINT32_MAX;
             e!=UINT32_MAX;e=owner->final.edges[e].next) {
            if (e>=owner->final.edge_count) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(owner->read.work,2)) return XR_XIR_BUDGET;
            EffectInvocationEdge edge=owner->final.edges[e];
            if (edge.caller!=node || edge.target>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
            if (!edge.latent) status=effect_invocation_join(owner,node,edge.target,edge.instruction,&changed);
            if (status!=XR_XIR_OK) break;
        }
        if (status!=XR_XIR_OK) return status;
        if (!changed) continue;
        for (uint32_t e=owner->read.consumers[node];e!=UINT32_MAX;e=owner->final.edges[e].consumer_next) {
            if (e>=owner->final.edge_count || owner->final.edges[e].target!=node) return XR_XIR_BAD_STRUCTURE;
            if (!xir_compile_work(owner->read.work,1)) return XR_XIR_BUDGET;
            status=effect_invocation_enqueue(owner,owner->final.edges[e].caller);
            if (status!=XR_XIR_OK) return status;
        }
    }
    for (uint32_t n=0;n<owner->final.count;++n) {
        if (!xir_compile_work(owner->read.work,2)) return XR_XIR_BUDGET;
        if (!owner->states[n].expanded || owner->states[n].queued) return XR_XIR_BAD_STRUCTURE;
    }
    return effect_invocation_coverage_solve(owner);
}

/* This is the existing exhaustive local opcode classifier. Only indirect
 * calls are removed from its advertisement seed and consumed as authenticated
 * invocation equations instead. Requirement UNKNOWN remains intrinsic. */
static XrXirStatus effect_invocation_intrinsics(EffectInvocationWorkspace *owner) {
    if (!owner || !effect_body_material_shape(owner->final.material) || owner->final.local_roots ||
        owner->final.local_witnesses || !owner->final.material->local_roots || !owner->final.material->local_witnesses)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=owner->read.body.domain.function_count;
    if ((uint64_t)count>SIZE_MAX/sizeof(*owner->final.local_roots) ||
        (uint64_t)count*2>SIZE_MAX/sizeof(*owner->final.local_witnesses)) return XR_XIR_BUDGET;
    /* The generation owns these immutable seeds before any equation is built.
     * The equation owner dies before its transferred material on every path. */
    if (!xir_compile_work(owner->read.work,sizeof(owner->final.local_roots)+sizeof(owner->final.local_witnesses)))
        return XR_XIR_BUDGET;
    owner->final.local_roots=owner->final.material->local_roots;
    owner->final.local_witnesses=owner->final.material->local_witnesses;
    return XR_XIR_OK;
}

/* Root formal seeds do not certify an actual binding. Scoped Cell formals
 * are explicit dependencies, and REF callable formals are deferred symbols.
 * Every other fixed callable retains its complete declared advertisement. */
static XrXirStatus effect_invocation_root_input(EffectInvocationWorkspace *owner,
    uint32_t function,uint64_t **output) {
    if (!output || *output || function>=owner->read.body.domain.function_count || !effect_contracts_read(owner->effects))
        return XR_XIR_BAD_STRUCTURE;
    if (!owner->final.root_spaces || owner->final.root_spaces[function]>=owner->read.body.domain.function_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(&owner->read,owner->final.root_spaces[function],&basis);
    if (status!=XR_XIR_OK) return status;
    EffectBodyFunctionHead head={0};status=effect_invocation_head(&owner->read,function,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionHead *body=&head;
    uint64_t words=(uint64_t)body->parameter_count*basis.row_words;
    if (words>SIZE_MAX/sizeof(uint64_t)) return XR_XIR_BUDGET;
    uint64_t *input=words ? xir_compile_calloc(owner->read.work,(size_t)words,sizeof(*input),&status) : NULL;
    if (words && !input) return status;
    if (effect_contracts_read(owner->effects)[function].parameter_count!=body->parameter_count ||
        (body->parameter_count && (!body->parameters || !effect_contracts_read(owner->effects)[function].parameters))) {
        xr_compile_resources_free(input);return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t p=0;p<body->parameter_count && status==XR_XIR_OK;++p) {
        if (!xir_compile_work(owner->read.work,3)) { status=XR_XIR_BUDGET;break; }
        uint64_t *row=input+(size_t)p*basis.row_words;
        if (xr_xir_type_is_cell(owner->read.body.domain.types,body->parameters[p])) {
            uint32_t known=basis.parameters+2;
            row[basis.fn_words+p/64]|=UINT64_C(1)<<(p%64);
            row[basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
            continue;
        }
        if (xr_xir_type_is_class(owner->read.body.domain.types,body->parameters[p])) {
            uint32_t unknown=basis.parameters+1,known=unknown+1;
            row[basis.fn_words+unknown/64]|=UINT64_C(1)<<(unknown%64);
            row[basis.fn_words+known/64]|=UINT64_C(1)<<(known%64);
            continue;
        }
        const XrXirTypeNode *signature=xr_xir_callable_signature(owner->read.body.domain.types,body->parameters[p]);
        if (!signature) continue;
        bool ref=false;
        for (uint32_t a=0;a<signature->parameter_count;++a) {
            if (!xir_compile_work(owner->read.work,1)) { status=XR_XIR_BUDGET;break; }
            if (!xr_xir_callable_parameter_storage_valid(owner->read.body.domain.types,&signature->parameters[a])) {
                status=XR_XIR_BAD_TYPE;break;
            }
            if (signature->parameters[a].mode==XR_PARAM_REF) ref=true;
        }
        if (status!=XR_XIR_OK) break;
        uint32_t mask=0;
        status=effect_invocation_bounds_mask(owner->read.work,owner->read.declared,function,p,&mask);
        if (status!=XR_XIR_OK) break;
        const XrXirEffectParameter *parameter=&effect_contracts_read(owner->effects)[function].parameters[p];
        if (mask && ((ref && (mask&XR_XIR_CALLABLE_ROOT_UNRESOLVED)) ||
            parameter->kind==XR_XIR_EFFECT_PARAMETER_VARIABLE)) {
            uint32_t bit=owner->final.site_count+p;
            row[bit/64]|=UINT64_C(1)<<(bit%64);
            if (ref && (mask&XR_XIR_CALLABLE_ROOT_REQUIRED)) {
                uint32_t opaque=owner->final.site_count+basis.parameters;
                row[opaque/64]|=UINT64_C(1)<<(opaque%64);
                row[(opaque+1)/64]|=UINT64_C(1)<<((opaque+1)%64);
            }
        } else {
            uint32_t opaque=owner->final.site_count+basis.parameters;
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
static XrXirStatus effect_invocation_root_spaces(EffectInvocationWorkspace *owner) {
    uint32_t count=owner->read.body.domain.function_count;
    if ((uint64_t)count>SIZE_MAX/sizeof(*owner->final.root_spaces)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    owner->final.root_spaces=xir_compile_alloc(owner->read.work,(size_t)count*sizeof(*owner->final.root_spaces),&status);
    if (!owner->final.root_spaces) return status;
    uint32_t shared=UINT32_MAX;
    for (uint32_t f=0;f<count;++f) {
        EffectBodyFunctionHead head={0};status=effect_invocation_head(&owner->read,f,&head);
        if (status!=XR_XIR_OK) return status;
        const EffectBodyFunctionHead *function=&head;bool tracked=false;
        for (uint32_t p=0;p<function->parameter_count;++p) {
            if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
            XrXirType type=function->parameters[p];
            if (xr_xir_callable_signature(owner->read.body.domain.types,type) || xr_xir_type_is_cell(owner->read.body.domain.types,type) ||
                xr_xir_type_is_class(owner->read.body.domain.types,type))
                tracked=true;
        }
        if (!xir_compile_work(owner->read.work,2)) return XR_XIR_BUDGET;
        if (!tracked && shared==UINT32_MAX) shared=f;
        owner->final.root_spaces[f]=tracked?f:shared;
    }
    return XR_XIR_OK;
}

/* An empty transfer relation is proved from this request's actual rows. The
 * local effects may still contain initializer or slot authority and errors.
 * This temporary receiver is neither an advertisement nor a saved verdict. */
typedef struct EffectInvocationLocalInput {
    XrXirRootEffects facts;
    XrXirRootEffectWitness witnesses[2];
} EffectInvocationLocalInput;

/* Expression children precede their parent. A fresh prefix induction proves
 * each descriptor's exact parameter span without a heap callback. The cursor
 * belongs only to this uninterrupted read and never certifies a later request. */
typedef struct EffectInvocationLocalRead {
    const XrXirCompileContext *work;
    const EffectBodyMaterial *body;
    uint32_t verified_types;
} EffectInvocationLocalRead;

static XrXirStatus effect_invocation_local_type_edge(EffectInvocationLocalRead *read,
    XrXirType type,uint32_t earlier,uint32_t *span) {
    if (!xir_compile_work(read->work,1)) return XR_XIR_BUDGET;
    uint32_t id=(uint32_t)type;
    if (id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
        *span=id-XR_XIR_TYPE_PARAMETER_BASE+1;return XR_XIR_OK;
    }
    const XrXirTypeNode *node=xr_xir_type_node(read->body->domain.types,type);
    if (node) {
        if (id-XR_XIR_CONSTRUCTED_TYPE_BASE>=earlier) return XR_XIR_BAD_TYPE;
        *span=node->parameter_span;return XR_XIR_OK;
    }
    if (type!=XR_XIR_UNIT && type!=XR_XIR_BOOL && type!=XR_XIR_RUNE &&
        !xr_xir_type_is_number(type) && type!=XR_XIR_STRING && type!=XR_XIR_ERROR &&
        type!=XR_XIR_PANIC_INFO) return XR_XIR_BAD_TYPE;
    *span=0;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_type(EffectInvocationLocalRead *read,XrXirType type) {
    const XrXirTypes *types=read->body->domain.types;
    const XrXirTypeNode *root=xr_xir_type_node(types,type);
    if (!root) {
        uint32_t span=0;
        XrXirStatus status=effect_invocation_local_type_edge(read,type,0,&span);
        return status!=XR_XIR_OK?status:(span?XR_XIR_BAD_TYPE:XR_XIR_OK);
    }
    uint32_t count=(uint32_t)type-XR_XIR_CONSTRUCTED_TYPE_BASE+1;
    for (uint32_t i=read->verified_types;i<count;++i) {
        if (!xir_compile_work(read->work,4)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node=&types->nodes[i];
        const XrXirType *arguments=NULL;
        uint32_t arity=0,span=0;
        XrXirStatus status=XR_XIR_OK;
        if (node->kind==XR_XIR_TYPE_NOMINAL) {
            const XrXirNominalTable *table=types->nominals;
            if (!table || node->nominal.declaration>=table->count ||
                (node->nominal.argument_count && !node->nominal.arguments))
                return XR_XIR_BAD_TYPE;
            const XrXirNominalDeclaration *declaration=table->declarations?
                &table->declarations[node->nominal.declaration]:NULL;
            if (declaration && (declaration->parameter_count!=node->nominal.argument_count ||
                (declaration->parameter_count && !declaration->constraints))) return XR_XIR_BAD_TYPE;
            if (!declaration && !table->identities) return XR_XIR_BAD_TYPE;
            arguments=node->nominal.arguments;arity=node->nominal.argument_count;
        } else if (node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_CELL ||
            node->kind==XR_XIR_TYPE_NULLABLE || node->kind==XR_XIR_TYPE_ATOMIC ||
            node->kind==XR_XIR_TYPE_TASK || node->kind==XR_XIR_TYPE_SET ||
            node->kind==XR_XIR_TYPE_MAP) {
            status=effect_invocation_local_type_edge(read,node->element,i,&span);
            if (status==XR_XIR_OK && node->kind==XR_XIR_TYPE_MAP) {
                uint32_t child=0;
                status=effect_invocation_local_type_edge(read,node->result,i,&child);
                if (child>span) span=child;
            }
        } else if (node->kind==XR_XIR_TYPE_CALLABLE || node->kind==XR_XIR_TYPE_TUPLE) {
            if (node->parameter_count && !node->parameters) return XR_XIR_BAD_STRUCTURE;
            if (node->kind==XR_XIR_TYPE_CALLABLE)
                status=effect_invocation_local_type_edge(read,node->result,i,&span);
            for (uint32_t a=0;a<node->parameter_count && status==XR_XIR_OK;++a) {
                uint32_t child=0;
                status=effect_invocation_local_type_edge(read,node->parameters[a].type,i,&child);
                if (child>span) span=child;
            }
        } else return XR_XIR_BAD_TYPE;
        for (uint32_t a=0;a<arity && status==XR_XIR_OK;++a) {
            uint32_t child=0;
            status=effect_invocation_local_type_edge(read,arguments[a],i,&child);
            if (child>span) span=child;
        }
        if (status!=XR_XIR_OK) return status;
        if (node->parameter_span!=span) return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(read->work,sizeof(read->verified_types))) return XR_XIR_BUDGET;
        read->verified_types=i+1;
    }
    return root->parameter_span?XR_XIR_BAD_TYPE:XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_operation(EffectInvocationLocalRead *read,
    uint32_t f,const XrXirInstruction *op,bool *empty) {
    const XrXirCompileContext *work=read->work;const EffectBodyMaterial *body=read->body;
    if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
    if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus typed=effect_invocation_local_type(read,op->type);
    if (typed!=XR_XIR_OK) return typed;
    uint32_t mask=effect_physical_opcode(op->op);
    if (xr_xir_callable_signature(body->domain.types,op->type) ||
        xr_xir_type_is_cell(body->domain.types,op->type) ||
        xr_xir_type_is_class(body->domain.types,op->type) ||
        xr_xir_type_span(body->domain.types,op->type) ||
        mask&(EFFECT_PHYSICAL_REFERENCE|EFFECT_PHYSICAL_TRANSFER|
            EFFECT_PHYSICAL_INVOCATION|EFFECT_PHYSICAL_DIRECT|EFFECT_PHYSICAL_GO|
            EFFECT_PHYSICAL_CONTROL|EFFECT_PHYSICAL_SLOT_PLACE) ||
        op->op==XR_XIR_CELL_PLACE || op->op==XR_XIR_FIELD_PLACE ||
        op->op==XR_XIR_INDEX_PLACE || op->op==XR_XIR_OBJECT_PLACE) {
        if (!xir_compile_work(work,sizeof(*empty))) return XR_XIR_BUDGET;
        *empty=false;return XR_XIR_OK;
    }
    if (mask&EFFECT_PHYSICAL_BARRIER) {
        const EffectBodyFunction *head=&body->functions[f];
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!xr_xir_op_uses_operand_table(op->op) || op->args[1]) {
            if (!xir_compile_work(work,sizeof(*empty))) return XR_XIR_BUDGET;
            *empty=false;return XR_XIR_OK;
        }
        if (op->args[0]>head->operand_count) return XR_XIR_BAD_STRUCTURE;
        /* The existing barrier core takes args[1] for a table operation.
         * Its zero count still requires the genuine operand-table bound.
         * Cell creation was rejected by its actual result type. */
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_run(EffectInvocationLocalRead *read,
    uint32_t f,const EffectBodyRun *run,EffectInvocationLocalInput *output,bool *empty) {
    const XrXirCompileContext *work=read->work;const EffectBodyMaterial *body=read->body;
    if (!xir_compile_work(work,4+sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t semantic=UINT32_C(3);
    /* Slot reads consume the complete signed slot identity. Every other
     * eligible opcode enters the original exhaustive seed kernel unchanged. */
    if (run->base.op==XR_XIR_SLOT_LOAD) semantic|=UINT32_C(192);
    if (effect_physical_opcode(run->base.op)&EFFECT_PHYSICAL_BARRIER)
        semantic|=UINT32_C(12)|UINT32_C(192);
    uint32_t points=(run->mask&semantic)?run->count:1;
    for (uint32_t offset=0;offset<points;++offset) {
        if (!xir_compile_work(work,2+sizeof(void *))) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=&run->base;
        XrXirStatus status=XR_XIR_OK;
        if (points>1) {
            EffectBodyRead body_read=effect_body_compile_read(work);
            status=effect_body_instruction_ref_value(&body_read,run,run->first+offset,&storage);
            op=&storage;
        }
        if (status==XR_XIR_OK) status=effect_invocation_local_operation(read,f,op,empty);
        if (status!=XR_XIR_OK || !*empty) return status;
        if (op->op==XR_XIR_SLOT_LOAD && (!body->declarations || !body->declarations->slots ||
            op->immediate<0 || (uint64_t)op->immediate>=body->declarations->slot_count))
            return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,sizeof(EffectRootSeedRecord))) return XR_XIR_BUDGET;
        EffectRootSeedRecord seed={&body->domain,op,f,run->first+offset,XR_XIR_UNIT};
        if (op->op==XR_XIR_SLOT_LOAD && !xir_compile_work(work,
            sizeof(output->facts)+sizeof(output->witnesses[0]))) return XR_XIR_BUDGET;
        status=effect_root_seed_output(&seed,&output->facts,&output->witnesses[0],
            &output->witnesses[1],work);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_head(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,uint32_t f,bool *empty) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(body) ||
        body->resources!=work->resources || !empty || f>=effect_body_count(body))
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&body->functions[f];
    if (!xir_compile_work(work,11+sizeof(*empty))) return XR_XIR_BUDGET;
    if ((head->name_length && !head->name) || (head->parameter_count && !head->parameters) ||
        (head->operand_count && !head->operands) || (head->block_count && !head->blocks) ||
        head->run_count>head->run_capacity || head->run_capacity>head->instruction_count ||
        !!head->runs!=!!head->run_capacity || !!head->run_count!=!!head->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    *empty=true;
    /* A formal relation needs the ordinary complete binding algorithm. This
     * restriction is dimensional, never a promise about the function's effect. */
    if (head->parameter_count || xr_xir_callable_signature(body->domain.types,head->result) ||
        xr_xir_type_is_cell(body->domain.types,head->result) ||
        xr_xir_type_is_class(body->domain.types,head->result) ||
        xr_xir_type_span(body->domain.types,head->result)) {
        if (!xir_compile_work(work,sizeof(*empty))) return XR_XIR_BUDGET;
        *empty=false;return XR_XIR_OK;
    }
    if (body->generics && body->generics[f].parameter_count) {
        if (!xir_compile_work(work,sizeof(*empty))) return XR_XIR_BUDGET;
        *empty=false;return XR_XIR_OK;
    }
    return XR_XIR_OK;
}

/* The same request consumes a structurally admitted head immediately.
 * No callback, retained verdict or permission flag separates its two parts. */
static XrXirStatus effect_invocation_local_input_body(EffectInvocationLocalRead *read,
    uint32_t f,EffectInvocationLocalInput *output,bool *empty) {
    const XrXirCompileContext *work=read->work;const EffectBodyMaterial *body=read->body;
    const EffectBodyFunction *head=&body->functions[f];
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectInvocationLocalInput){0};
    XrXirStatus typed=effect_invocation_local_type(read,head->result);
    if (typed!=XR_XIR_OK) return typed;
    if (body->declarations) {
        const XrXirDeclarations *decls=body->declarations;
        if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
        if (!decls->functions || !decls->modules || decls->functions[f].module>=decls->module_count)
            return XR_XIR_BAD_STRUCTURE;
        if (decls->modules[decls->functions[f].module].initializer==f) {
            if (!xir_compile_work(work,sizeof(output->facts)+sizeof(output->witnesses[0])))
                return XR_XIR_BUDGET;
            output->facts.requires_root=true;
            output->witnesses[0]=(XrXirRootEffectWitness){XR_XIR_ROOT_CAUSE_INITIALIZER,
                UINT32_MAX,UINT32_MAX,UINT32_MAX,0};
        }
    }
    if (!xir_compile_work(work,sizeof(EffectBodyRead)+sizeof(EffectBodyRecordCursor))) return XR_XIR_BUDGET;
    EffectBodyRead body_read=effect_body_compile_read(work);EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t r=0;r<head->run_count;++r) {
        const EffectBodyRun *run=NULL;
        XrXirStatus status=effect_body_range_ref(&body_read,head,&cursor,&run);
        if (status==XR_XIR_OK) status=effect_invocation_local_run(read,f,run,output,empty);
        if (status!=XR_XIR_OK || !*empty) return status;
    }
    if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
    return cursor.first==head->instruction_count && cursor.run==head->run_count?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* A failed mathematical classification leaves the generating owner untouched.
 * No failed callback or malformed range is converted into the normal path. */
static XrXirStatus effect_invocation_local_inputs(const XrXirCompileContext *work,
    const EffectBodyMaterial *body,EffectInvocationLocalInput **output) {
    if (!output || *output || !xir_compile_context_valid(work) || !effect_body_material_shape(body) ||
        body->resources!=work->resources) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=effect_body_count(body);XrXirStatus status=XR_XIR_OK;
    if ((uint64_t)count>SIZE_MAX/sizeof(**output)) return XR_XIR_BUDGET;
    /* The original count branch precedes one allocation. An empty domain
     * publishes the same empty output; no result storage is created. */
    if (!count) {
        if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
        *output=NULL;return XR_XIR_OK;
    }
    bool empty=false;status=effect_invocation_local_head(work,body,0,&empty);
    if (status!=XR_XIR_OK || !empty) return status;
    EffectInvocationLocalInput *inputs=xir_compile_alloc(work,(size_t)count*sizeof(*inputs),&status);
    if (!inputs) return status;
    EffectInvocationLocalRead read={work,body,0};
    status=effect_invocation_local_input_body(&read,0,&inputs[0],&empty);
    if (status!=XR_XIR_OK || !empty) { xr_compile_resources_free(inputs);return status; }
    for (uint32_t f=1;f<count;++f) {
        bool supported=false;status=effect_invocation_local_head(work,body,f,&supported);
        if (status==XR_XIR_OK && supported)
            status=effect_invocation_local_input_body(&read,f,&inputs[f],&supported);
        if (status!=XR_XIR_OK || !supported) { xr_compile_resources_free(inputs);return status; }
    }
    if (!xir_compile_work(work,sizeof(*output))) { xr_compile_resources_free(inputs);return XR_XIR_BUDGET; }
    *output=inputs;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_node_write(EffectInvocationWorkspace *owner,
    uint32_t function,const EffectInvocationLocalInput *input) {
    const EffectBodyFunction *head=&owner->final.material->functions[function];
    if (!owner->states || head->parameter_count || owner->final.count!=function)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t words=((uint64_t)head->instruction_count+63)/64;
    if (words>(SIZE_MAX-6*sizeof(EffectInvocationCause))/(2*sizeof(uint64_t))) return XR_XIR_BUDGET;
    size_t bytes=6*sizeof(EffectInvocationCause)+(size_t)words*2*sizeof(uint64_t);
    XrXirStatus status=XR_XIR_OK;
    EffectInvocationCause *causes=xir_compile_alloc(owner->read.work,bytes,&status);
    if (!causes) return status;
    if (!xir_compile_work(owner->read.work,bytes+sizeof(EffectInvocationNode)+
        3*sizeof(uint32_t)+sizeof(owner->final.count)+sizeof(*owner->states))) {
        xr_compile_resources_free(causes);return XR_XIR_BUDGET;
    }
    uint32_t mask=(input->facts.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (input->facts.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    for (uint32_t local=0;local<2;++local) for (uint32_t a=0;a<3;++a) {
        EffectInvocationCause *cause=&causes[local*3+a];
        *cause=(EffectInvocationCause){.next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
        if (a<2 && (mask&(a?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED))) {
            if (!xir_compile_work(owner->read.work,sizeof(cause->witness)+sizeof(cause->distance))) {
                xr_compile_resources_free(causes);return XR_XIR_BUDGET;
            }
            cause->witness=input->witnesses[a];cause->distance=0;
        }
    }
    uint64_t *values=words?(uint64_t *)(causes+6):NULL;
    if (words) memset(values,0,(size_t)words*2*sizeof(*values));
    owner->final.nodes[function]=(EffectInvocationNode){.root=0,.body=function,
        .instruction_count=head->instruction_count,.intrinsic_mask=mask,.local_mask=mask,
        .cause_count=3,.causes=causes,.cells=values,.local_cells=values?values+words:NULL,
        .edge_head=UINT32_MAX};
    owner->states[function]=(EffectInvocationState){.expanded=true};
    owner->final.roots[function]=function;owner->final.root_spaces[function]=0;
    owner->read.consumers[function]=UINT32_MAX;++owner->final.count;return XR_XIR_OK;
}

/* These are real final nodes of the same equations. No queue entries or
 * transient origin rows exist when the independently derived relation is empty. */
static XrXirStatus effect_invocation_local_nodes(EffectInvocationWorkspace *owner,
    const EffectInvocationLocalInput *inputs) {
    if (!owner || !inputs || !owner->read.work || owner->final.count || owner->final.nodes || owner->final.roots ||
        owner->final.root_spaces || owner->read.consumers || owner->states || !owner->final.material->local_roots ||
        !owner->final.material->local_witnesses) return XR_XIR_BAD_STRUCTURE;
    uint32_t count=effect_body_count(owner->final.material);XrXirStatus status=XR_XIR_OK;
    if (count>owner->maximum || (uint64_t)count>SIZE_MAX/sizeof(*owner->final.nodes)) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->read.work,5*sizeof(void *))) return XR_XIR_BUDGET;
    owner->final.nodes=xir_compile_alloc(owner->read.work,(size_t)count*sizeof(*owner->final.nodes),&status);
    if (status==XR_XIR_OK) owner->final.roots=xir_compile_alloc(owner->read.work,(size_t)count*sizeof(*owner->final.roots),&status);
    if (status==XR_XIR_OK) owner->final.root_spaces=xir_compile_alloc(owner->read.work,(size_t)count*sizeof(*owner->final.root_spaces),&status);
    if (status==XR_XIR_OK) owner->read.consumers=xir_compile_alloc(owner->read.work,
        (size_t)count*(sizeof(*owner->read.consumers)+sizeof(*owner->states)),&status);
    if (status!=XR_XIR_OK) return status;
    owner->states=(EffectInvocationState *)(owner->read.consumers+count);
    if (!xir_compile_work(owner->read.work,sizeof(owner->final.capacity)+
        sizeof(owner->final.local_roots)+sizeof(owner->final.local_witnesses))) return XR_XIR_BUDGET;
    owner->final.capacity=count;owner->final.local_roots=owner->final.material->local_roots;
    owner->final.local_witnesses=owner->final.material->local_witnesses;
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(owner->read.work,2+2*5)) return XR_XIR_BUDGET;
        if (inputs[f].facts.requires_root!=owner->final.local_roots[f].requires_root ||
            inputs[f].facts.unresolved!=owner->final.local_roots[f].unresolved ||
            !effect_invocation_witness_equal(&inputs[f].witnesses[0],&owner->final.local_witnesses[f]) ||
            !effect_invocation_witness_equal(&inputs[f].witnesses[1],&owner->final.local_witnesses[count+f]))
            return XR_XIR_BAD_STRUCTURE;
        status=effect_invocation_local_node_write(owner,f,&inputs[f]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Coordinates and every zero relation are compared to a freshly derived
 * actual input. Sparse inventories only locate; none authorizes the relation. */
static XrXirStatus effect_invocation_local_node_match(const XrXirCompileContext *work,
    const EffectInvocationFinal *owner,uint32_t f,const EffectInvocationLocalInput *input) {
    const EffectInvocationNode *node=&owner->nodes[f];
    const EffectBodyFunction *head=&owner->material->functions[f];
    uint32_t mask=(input->facts.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (input->facts.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    if (!xir_compile_work(work,32)) return XR_XIR_BUDGET;
    if (head->parameter_count || node->root || node->body!=f || node->parameter_count ||
        node->instruction_count!=head->instruction_count || owner->roots[f]!=f || owner->root_spaces[f] ||
        node->deferred || node->local_deferred ||
        node->intrinsic_mask!=mask || node->local_mask!=mask || node->input || node->parameter_coverage ||
        node->producer_count || node->producer_capacity || node->producers || node->return_coverage ||
        node->class_count || node->classes || node->class_return_mask || node->class_return_rank ||
        node->edge_head!=UINT32_MAX || !effect_invocation_node_shape(node,0,head->instruction_count))
        return XR_XIR_BAD_STRUCTURE;
    uint64_t words=((uint64_t)head->instruction_count+63)/64;
    if (words && node->local_cells!=node->cells+words) return XR_XIR_BAD_STRUCTURE;
    for (uint64_t w=0;w<words;++w) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (node->cells[w] || node->local_cells[w]) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t local=0;local<2;++local) for (uint32_t atom=0;atom<3;++atom) {
        const EffectInvocationCause *cause=&node->causes[local*3+atom];
        bool active=atom<2 && (mask&(atom?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED));
        if (!xir_compile_work(work,9)) return XR_XIR_BUDGET;
        if (cause->next!=UINT32_MAX || cause->edge!=UINT32_MAX ||
            cause->distance!=(active?0:UINT32_MAX)) return XR_XIR_BAD_STRUCTURE;
        if (active) {
            if (!effect_invocation_witness_equal(&cause->witness,&input->witnesses[atom]))
                return XR_XIR_BAD_STRUCTURE;
        } else if (cause->witness.cause || cause->witness.instruction || cause->witness.callee ||
            cause->witness.slot || cause->witness.distance) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_equations_shape(const XrXirCompileContext *work,
    const EffectInvocationFinal *owner) {
    if (!xir_compile_context_valid(work) || !owner || !effect_body_material_shape(owner->material) ||
        owner->material->resources!=work->resources || !owner->nodes || !owner->roots ||
        !owner->root_spaces) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,18)) return XR_XIR_BUDGET;
    uint32_t count=effect_body_count(owner->material);
    if (owner->count!=count || owner->count>owner->capacity || (uint64_t)owner->capacity>(uint64_t)work->limits.functions*work->limits.instructions ||
        owner->site_count || owner->binding_count || owner->sites || owner->deferred_count ||
        owner->deferred_capacity || owner->deferred_calls || owner->edge_count || owner->edge_capacity || owner->edges) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_equations_match(const XrXirCompileContext *work,
    const EffectInvocationFinal *owner,const EffectInvocationLocalInput *inputs) {
    if (!inputs) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus shape=effect_invocation_local_equations_shape(work,owner);
    if (shape!=XR_XIR_OK) return shape;
    uint32_t count=effect_body_count(owner->material);
    for (uint32_t f=0;f<count;++f) {
        XrXirStatus status=effect_invocation_local_node_match(work,owner,f,&inputs[f]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_local_result_new(const XrXirCompileContext *work,
    const EffectInvocationWorkspace *owner,const EffectInvocationLocalInput *inputs,
    EffectFinalFunctions **output);

/* Private creation runs inside the existing formula owner after full source,
 * body, domain, physical-role and construction verification. The structural
 * limit product bounds actual equation IDs; the fixed bit basis proves finite
 * state independently of that practical admission limit. */
static XrXirStatus effect_invocations_create(const XrXirCompileContext *work,
    const EffectBodyRequest *body_request,XrXirEffects *effects,EffectGraph *graph,
    const EffectInvocationDeclaredBounds *declared,EffectInvocationWorkspace **output) {
    if (!output || *output || !graph || !effects ||
        !effect_body_request_owned_matches(work,body_request,graph->material) ||
        !xir_effects_context_matches(work,effects,body_request->domain.function_count) || !effect_contracts_read(effects) ||
        !declared || declared!=graph->material->declared || declared->resources!=work->resources ||
        declared->count!=body_request->domain.function_count || !declared->functions ||
        graph->physical!=graph->material->physical || !graph->material->cells)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,24+sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t functions=body_request->domain.function_count;
    uint64_t maximum=(uint64_t)work->limits.functions*work->limits.instructions;
    if (maximum>UINT32_MAX) maximum=UINT32_MAX;
    if (!maximum || maximum<functions) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    /* The aggregate initializes every live and unused field once. It needs
     * no preceding calloc and cannot leave an uninitialized capacity tail. */
    EffectInvocationWorkspace *owner=xir_compile_alloc(work,sizeof(*owner),&status);
    if (!owner) return status;
    if (!xir_compile_work(work,sizeof(*owner))) {
        xr_compile_resources_free(owner);return XR_XIR_BUDGET; }
    *owner=(EffectInvocationWorkspace){.effects=effects,.graph=graph,.maximum=(uint32_t)maximum,
        .read={.work=work,.final=&owner->final,.effects=effects,.declared=declared,.body=*body_request,
            .replay_scratch=&owner->replay_scratch},
        .final={.material=graph->material,.physical=graph->material->physical,.cells=graph->material->cells}};
    EffectInvocationLocalInput *local_inputs=NULL;
    status=effect_invocation_local_inputs(work,owner->final.material,&local_inputs);
    if (status!=XR_XIR_OK) { effect_invocation_free(owner);return status; }
    if (local_inputs) {
        status=effect_invocation_local_nodes(owner,local_inputs);
        if (status==XR_XIR_OK)
            status=effect_invocation_local_result_new(work,owner,local_inputs,&owner->prepared_result);
        xr_compile_resources_free(local_inputs);
        if (status!=XR_XIR_OK) { effect_invocation_free(owner);return status; }
        *output=owner;return XR_XIR_OK;
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<functions;++f) {
        bool supported=false;status=effect_invocation_producer_return(owner,f,&supported);
        owner->read.producer_enabled|=supported;
    }

    if (status==XR_XIR_OK) {
        for (uint32_t f=0;f<owner->final.physical->count;++f) {
            if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
            EffectBodyFunctionHead head={0};status=effect_invocation_head(&owner->read,f,&head);
            if (status!=XR_XIR_OK) break;
            owner->read.class_enabled|=owner->final.physical->functions[f].classes!=0 ||
                xr_xir_type_is_class(owner->read.body.domain.types,head.result);
        }
    }
    if (status==XR_XIR_OK && owner->read.class_enabled) {
        EffectBodyRequest body=effect_invocation_body(&owner->read);
        status=effect_invocation_class_cells_match(&owner->read,&body);
    }
    if (status==XR_XIR_OK) status=effect_invocation_root_spaces(owner);
    if (status==XR_XIR_OK) status=effect_invocation_sites(owner);
    if (status==XR_XIR_OK) status=effect_invocation_intrinsics(owner);
    if (status==XR_XIR_OK) {
        if ((uint64_t)functions>SIZE_MAX/sizeof(*owner->final.roots)) status=XR_XIR_BUDGET;
        else owner->final.roots=xir_compile_alloc(work,(size_t)functions*sizeof(*owner->final.roots),&status);
        if (status==XR_XIR_OK && !owner->final.roots) status=XR_XIR_OUT_OF_MEMORY;
    }
    for (uint32_t f=0;status==XR_XIR_OK && f<functions;++f) {
        uint64_t *input=NULL;
        status=effect_invocation_root_input(owner,f,&input);
        uint32_t node=UINT32_MAX;
        if (status==XR_XIR_OK) status=effect_invocation_intern(owner,owner->final.root_spaces[f],f,input,&node);
        xr_compile_resources_free(input);
        if (status==XR_XIR_OK) {
            if (!xir_compile_work(work,1)) status=XR_XIR_BUDGET;
            else owner->final.roots[f]=node;
        }
    }
    if (status!=XR_XIR_OK) { effect_invocation_free(owner);return status; }
    *output=owner;return XR_XIR_OK;
}


/* The receiving request borrows the actual Final. Its reverse index is fresh
 * verification scratch and never contains a solver queue or copied inventory. */
static XrXirStatus effect_invocation_read_open(const XrXirCompileContext *work,
    const XrXirEffects *effects,const EffectInvocationDeclaredBounds *declared,
    const EffectInvocationFinal *final,EffectInvocationScratch **replay_scratch,EffectInvocationRead *read) {
    if (!xir_compile_context_valid(work) || !final || !read || !replay_scratch || *replay_scratch ||
        !effect_body_material_shape(final->material) || final->material->resources!=work->resources ||
        read->work || read->consumers || read->final || read->replay_scratch ||
        !final->physical || final->physical!=final->material->physical ||
        final->physical->resources!=work->resources ||
        final->physical->count!=effect_body_count(final->material) || !final->physical->functions ||
        !final->nodes || final->count>final->capacity || final->edge_count>final->edge_capacity ||
        (final->edge_count && !final->edges)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(*read))) return XR_XIR_BUDGET;
    *read=(EffectInvocationRead){.work=work,.effects=effects,.declared=declared,.final=final,
        .body=effect_body_owned_request(work,final->material),.replay_scratch=replay_scratch};
    for (uint32_t f=0;f<effect_body_count(final->material);++f) {
        bool supported=false;XrXirStatus status=effect_invocation_producer_return_read(read,&read->body,f,&supported);
        if (status!=XR_XIR_OK) return status;
        read->producer_enabled|=supported;
        read->class_enabled|=final->physical->functions[f].classes!=0 ||
            xr_xir_type_is_class(final->material->domain.types,final->material->functions[f].result);
    }
    if (!read->producer_enabled) return XR_XIR_OK;
    uint32_t count=final->count,edges=final->edge_count;
    uint32_t capacity=final->capacity,edge_capacity=final->edge_capacity;
    const EffectInvocationNode *nodes=final->nodes;const EffectInvocationEdge *links=final->edges;
    const EffectBodyMaterial *material=final->material;const EffectPhysicalDomain *physical=final->physical;
    const uint32_t *roots=final->roots,*root_spaces=final->root_spaces;
    if (!xir_compile_work(work,6*sizeof(void *)+4*sizeof(uint32_t))) return XR_XIR_BUDGET;
    if ((uint64_t)count*sizeof(*read->consumers)>SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    read->consumers=xir_compile_alloc(work,(size_t)count*sizeof(*read->consumers),&status);
    if (!read->consumers) return status;
    /* The allocator may change the descriptor. Frozen local extents qualify
     * only this request's allocation, never a callback's later dimensions. */
    if (!xir_compile_work(work,10)) status=XR_XIR_BUDGET;
    else if (final->count!=count || final->edge_count!=edges ||
        final->capacity!=capacity || final->edge_capacity!=edge_capacity ||
        final->nodes!=nodes || final->edges!=links || final->material!=material || final->physical!=physical ||
        final->roots!=roots || final->root_spaces!=root_spaces) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,(uint64_t)count*sizeof(*read->consumers)))
        status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) for (uint32_t n=0;n<count;++n) read->consumers[n]=UINT32_MAX;
    for (uint32_t e=0;e<edges && status==XR_XIR_OK;++e) {
        if (!xir_compile_work(work,5)) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationEdge *edge=&final->edges[e];
        if (edge->caller>=count || edge->target>=count ||
            edge->consumer_next!=read->consumers[edge->target]) { status=XR_XIR_BAD_STRUCTURE;break; }
        read->consumers[edge->target]=e;
    }
    if (status!=XR_XIR_OK) {
        xr_compile_resources_free(read->consumers);read->consumers=NULL;
    }
    return status;
}
static void effect_invocation_read_close(EffectInvocationRead *read) {
    effect_invocation_scratch_free(read);
    xr_compile_resources_free(read->consumers);read->consumers=NULL;
    read->replay_scratch=NULL;
}
