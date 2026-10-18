/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_class_verify.inc.c - Exact Class masks and descending complete-source ranks
 *
 * KEY CONCEPT:
 *   Complete physical membership, every source and every return are replayed.
 *   A zero rank, open formal or unknown carrier cannot prove a pure identity.
 */
static XrXirStatus effect_invocation_class_fact_match(const EffectInvocationRead *owner,
    const EffectInvocationClassFact *fact,const EffectInvocationClassSources *source,
    uint64_t limit) {
    if (!xir_compile_work(owner->work,6)) return XR_XIR_BUDGET;
    if (!fact || !source || !fact->mask || (fact->mask&~EFFECT_CLASS_VALID) ||
        !(fact->mask&EFFECT_CLASS_KNOWN) || !source->complete || !source->mask)
        return XR_XIR_BAD_STRUCTURE;
    if (source->mask!=fact->mask) return XR_XIR_BAD_STRUCTURE;
    if (fact->mask!=EFFECT_CLASS_KNOWN)
        return fact->rank?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    if (source->maximum>=limit || fact->rank<=source->maximum || fact->rank>limit)
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

/* A current type selects the same Class fact check for either body adapter.
 * The fact source and descending-rank proof remain independent of traversal. */
static XrXirStatus effect_invocation_class_member_match(EffectInvocationFlow *flow,
    uint32_t value,XrXirType type,uint32_t *at,uint64_t limit) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationClassDiscovery *rows=effect_invocation_class_discovery(owner,flow->node);
    if(!rows) return XR_XIR_BAD_STRUCTURE;
    if (!xr_xir_type_is_class(flow->body.domain.types,type)) {
        if (*at<rows->count && rows->facts[*at].value<=value) return XR_XIR_BAD_STRUCTURE;
        return XR_XIR_OK;
    }
    if (*at>=rows->count || !rows->facts || rows->facts[*at].value!=value)
        return XR_XIR_BAD_STRUCTURE;
    EffectInvocationClassSources sources={0};
    XrXirStatus status=effect_invocation_class_value(flow,value,&sources);
    if (status!=XR_XIR_OK) return status;
    return effect_invocation_class_fact_match(owner,&rows->facts[(*at)++],&sources,limit);
}

/* Constant current type columns prove a whole non-Class interval. Every
 * actual Class value still replays its unchanged sources and rank, and a
 * variable opcode/type column is read at each physical ordinal. */
static XrXirStatus effect_invocation_class_values_match(EffectInvocationFlow *flow,
    uint64_t limit) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationNode *node=&owner->final->nodes[flow->node];
    const EffectInvocationClassDiscovery *rows=effect_invocation_class_discovery(owner,flow->node);
    if(!rows) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,30+sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t at=0;
    if (rows->count && !rows->facts) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *material=owner->final->material;
    if (!effect_body_request_owned_matches(owner->work,&flow->body,material)) {
        for (uint32_t v=0;v<flow->values;++v) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            XrXirType type=0;XrXirStatus status=effect_invocation_flow_type(flow,v,&type);
            if (status==XR_XIR_OK) status=effect_invocation_class_member_match(flow,v,type,&at,limit);
            if (status!=XR_XIR_OK) return status;
        }
        return at==rows->count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    }
    if (node->body>=effect_body_count(material)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *body=&material->functions[node->body];
    if (!xir_compile_work(owner->work,7)) return XR_XIR_BUDGET;
    if (body->parameter_count!=flow->function->parameter_count ||
        body->instruction_count!=flow->function->instruction_count ||
        (uint64_t)body->parameter_count+body->instruction_count!=flow->values ||
        (body->parameter_count && !body->parameters)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<body->parameter_count;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        XrXirType type=0;XrXirStatus status=effect_invocation_flow_type(flow,p,&type);
        if (status==XR_XIR_OK) status=effect_invocation_class_member_match(flow,p,type,&at,limit);
        if (status!=XR_XIR_OK) return status;
    }
    if (!xir_compile_work(owner->work,sizeof(EffectBodyRecordCursor)+2*sizeof(uint32_t)))
        return XR_XIR_BUDGET;
    EffectBodyRecordCursor cursor={0,0,UINT32_MAX};
    for (uint32_t r=0;r<body->run_count;++r) {
        const EffectBodyRun *ref;XrXirStatus status=effect_body_range_ref(&flow->body.read,body,&cursor,&ref);
        if (status!=XR_XIR_OK) return status;
        uint32_t opcode=0,type=0;
        status=effect_body_run_word(&flow->body.read,ref,ref->first,0,&opcode);
        if (status==XR_XIR_OK) status=effect_body_run_word(&flow->body.read,ref,ref->first,1,&type);
        if (status!=XR_XIR_OK) return status;
        if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(owner->work,8)) return XR_XIR_BUDGET;
        if (!(ref->mask&3u) && !xr_xir_type_is_class(flow->body.domain.types,(XrXirType)type)) {
            uint64_t end=(uint64_t)body->parameter_count+ref->first+ref->count;
            if (at<rows->count && rows->facts[at].value<end) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        for (uint32_t i=ref->first;i<ref->first+ref->count;++i) {
            if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
            if (ref->mask&1u) status=effect_body_run_word(&flow->body.read,ref,i,0,&opcode);
            if (status==XR_XIR_OK && (ref->mask&2u))
                status=effect_body_run_word(&flow->body.read,ref,i,1,&type);
            if (status!=XR_XIR_OK) return status;
            if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            status=effect_invocation_class_member_match(flow,body->parameter_count+i,(XrXirType)type,&at,limit);
            if (status!=XR_XIR_OK) return status;
        }
    }
    return cursor.first==body->instruction_count && cursor.run==body->run_count &&
        at==rows->count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_invocation_class_body_match(EffectInvocationFlow *flow,
    uint64_t limit) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationClassDiscovery *rows=effect_invocation_class_discovery(owner,flow->node);
    if(!rows) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus membership=effect_invocation_class_values_match(flow,limit);
    if(membership!=XR_XIR_OK) return membership;
    if (!xr_xir_type_is_class(flow->body.domain.types,flow->function->result))
        return rows->return_mask || rows->return_rank?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    EffectInvocationClassSources sources={.complete=true};uint32_t returns=0;
    EffectBodyRecordCursor cursor;
    XrXirStatus sequence=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (sequence!=XR_XIR_OK) return sequence;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_1;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_1=effect_body_request_sequence_operation(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&body_op_1,&op);
        if (body_read_1!=XR_XIR_OK) return body_read_1;
        if (op->op!=XR_XIR_RETURN) continue;
        ++returns;XrXirStatus status=effect_invocation_class_source(flow,op->args[0],&sources);
        if (status!=XR_XIR_OK) return status;
    }
    sequence=effect_body_request_sequence_finish(&flow->body,flow->read->final->nodes[flow->node].body,&cursor);
    if (sequence!=XR_XIR_OK) return sequence;
    if (!returns) sources.mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;
    EffectInvocationClassFact returned={.mask=rows->return_mask,.rank=rows->return_rank};
    return effect_invocation_class_fact_match(owner,&returned,&sources,limit);
}

/* Root Class formals are the unchanged public UNKNOWN upper bound. Actual
 * executing nodes are distinct complete input keys, never a narrowed root. */
static XrXirStatus effect_invocation_class_root_match(const EffectInvocationRead *owner,
    const EffectBodyRequest *request,uint32_t function) {
    if (!owner->final->roots || !owner->final->root_spaces || function>=request->domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=owner->final->roots[function];
    if (selected>=owner->final->count || owner->final->nodes[selected].body!=function ||
        owner->final->nodes[selected].root!=owner->final->root_spaces[function]) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final->nodes[selected];
    EffectBodyFunctionSignature body_head={0};EffectBodyFunctionDimensions root_head={0};
    if (!xir_compile_work(owner->work,sizeof(*request))) return XR_XIR_BUDGET;
    EffectInvocationFlow flow={.read=owner,.node=selected,.body=*request};
    XrXirStatus status=effect_body_request_signature(request,function,&body_head);
    if (status==XR_XIR_OK) status=effect_body_request_dimensions(request,node->root,&root_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionSignature *body=&body_head;
    status=effect_invocation_basis_parameters(owner,root_head.parameter_count,&flow.basis);
    if (status!=XR_XIR_OK) return status;
    if (node->parameter_count!=body->parameter_count || (body->parameter_count && !body->parameters) ||
        (node->parameter_count && !node->input)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p=0;p<body->parameter_count;++p) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        if (!xr_xir_type_is_class(request->domain.types,body->parameters[p])) continue;
        if (node->root!=function) return XR_XIR_BAD_STRUCTURE;
        uint32_t mask=0;
        status=effect_invocation_class_input(&flow,node->input+(size_t)p*flow.basis.row_words,&mask);
        if (status!=XR_XIR_OK) return status;
        if (mask!=(EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN)) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_discovery_verify(const EffectInvocationWorkspace *workspace,
    const EffectBodyRequest *request) {
    const EffectInvocationRead *owner=workspace?&workspace->read:NULL;
    if(!owner || owner->discovery!=workspace || owner->roles.sealed || !owner->roles.construction)
        return XR_XIR_BAD_STRUCTURE;
    if (!owner || !request || !request->owner || !owner->work ||
        !request->domain.types) return XR_XIR_BAD_STRUCTURE;
    if (!owner->final->physical || owner->final->physical->resources!=owner->work->resources ||
        owner->final->physical->count!=request->domain.function_count ||
        (owner->final->physical->count && !owner->final->physical->functions)) return XR_XIR_BAD_STRUCTURE;
    bool enabled=false;
    for (uint32_t f=0;f<request->domain.function_count;++f) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        EffectBodyFunctionSignature head={0};
        XrXirStatus status=effect_body_request_signature(request,f,&head);
        if (status!=XR_XIR_OK) return status;
        enabled|=owner->final->physical->functions[f].classes!=0 ||
            xr_xir_type_is_class(request->domain.types,head.result);
    }
    if (enabled!=owner->class_enabled) return XR_XIR_BAD_STRUCTURE;
    if (!enabled) return XR_XIR_OK;

    uint64_t limit=0;XrXirStatus status=effect_invocation_class_cells_match(owner,request);
    if (status==XR_XIR_OK)
        status=effect_invocation_class_rank_limit_count(owner,request->domain.function_count,&limit);
    for (uint32_t f=0;status==XR_XIR_OK && f<request->domain.function_count;++f)
        status=effect_invocation_class_root_match(owner,request,f);
    for (uint32_t n=0;status==XR_XIR_OK && n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,4)) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationNode *node=&owner->final->nodes[n];
        const EffectInvocationClassDiscovery *rows=effect_invocation_class_discovery(owner,n);
        if (!rows || node->body>=request->domain.function_count ||
            rows->count!=owner->final->physical->functions[node->body].classes) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX,.precision=true};
        status=effect_invocation_discovery_origins_read(&flow,request);
        if (status==XR_XIR_OK) status=effect_invocation_class_body_match(&flow,limit);
        if (status==XR_XIR_OK) status=effect_invocation_class_terminals_match(&flow);
        xr_compile_resources_free(flow.memory);
    }
    return status;
}

static XrXirStatus effect_invocation_cell_terminals_read(const EffectInvocationRead *owner,
    const EffectBodyRequest *request) {
    if (!owner || !request || !request->owner || !owner->work ||
        !request->domain.types) return XR_XIR_BAD_STRUCTURE;
    if (!owner->final->physical || owner->final->physical->resources!=owner->work->resources ||
        owner->final->physical->count!=request->domain.function_count ||
        (owner->final->physical->count && !owner->final->physical->functions)) return XR_XIR_BAD_STRUCTURE;
    bool enabled=false;
    for (uint32_t f=0;f<request->domain.function_count;++f) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        EffectBodyFunctionHead head={0};
        XrXirStatus status=effect_body_request_head(request,f,&head);
        if (status!=XR_XIR_OK) return status;
        enabled|=owner->final->physical->functions[f].classes!=0 ||
            xr_xir_type_is_class(request->domain.types,head.result);
    }
    if (enabled!=owner->class_enabled) return XR_XIR_BAD_STRUCTURE;
    if (!enabled) return XR_XIR_OK;
    XrXirStatus status=effect_invocation_class_cells_match(owner,request);
    for (uint32_t f=0;status==XR_XIR_OK && f<request->domain.function_count;++f)
        status=effect_invocation_class_root_match(owner,request,f);
    for (uint32_t n=0;status==XR_XIR_OK && n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,4)) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationNode *node=&owner->final->nodes[n];
        if (node->body>=request->domain.function_count) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX,.precision=true};
        status=effect_invocation_final_head(&flow,request);
        if (status==XR_XIR_OK) status=effect_invocation_class_terminals_match(&flow);
    }
    return status;
}

static XrXirStatus effect_invocation_class_verify(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !effect_body_material_shape(owner->final.material) || !owner->read.work) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_class_discovery_verify(owner,&request);
}
