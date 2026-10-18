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

/* Each actual type is reread from this operation's complete physical copy.
 * Classification and every source/rank proof still run for each value. */
static XrXirStatus effect_invocation_class_body_match(EffectInvocationFlow *flow,
    uint64_t limit,const XrXirFunction *physical) {
    const EffectInvocationRead *owner=flow->read;
    const EffectInvocationNode *node=&owner->final->nodes[flow->node];
    uint32_t at=0;
    for (uint32_t v=0;v<flow->values;++v) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        XrXirType actual_type=0;
        XrXirStatus type_status=XR_XIR_OK;
        if (physical) {
            if ((uint64_t)physical->parameter_count+physical->instruction_count!=flow->values)
                type_status=XR_XIR_BAD_STRUCTURE;
            else if (!xir_compile_work(owner->work,4+sizeof(actual_type))) type_status=XR_XIR_BUDGET;
            else actual_type=xr_xir_operand_type(physical,v);
        } else type_status=effect_invocation_flow_type(flow,v,&actual_type);
        if (type_status!=XR_XIR_OK) return type_status;
        bool is_class=xr_xir_type_is_class(flow->body.domain.types,actual_type);
        if (!is_class) {
            if (at<node->class_count && node->classes[at].value<=v) return XR_XIR_BAD_STRUCTURE;
            continue;
        }
        if (at>=node->class_count || !node->classes || node->classes[at].value!=v)
            return XR_XIR_BAD_STRUCTURE;
        EffectInvocationClassSources sources={0};
        XrXirStatus status=effect_invocation_class_value(flow,v,&sources);
        if (status!=XR_XIR_OK) return status;
        status=effect_invocation_class_fact_match(owner,&node->classes[at++],&sources,limit);
        if (status!=XR_XIR_OK) return status;
    }
    if (at!=node->class_count) return XR_XIR_BAD_STRUCTURE;
    if (!xr_xir_type_is_class(flow->body.domain.types,flow->function->result))
        return node->class_return_mask || node->class_return_rank?XR_XIR_BAD_STRUCTURE:XR_XIR_OK;
    EffectInvocationClassSources sources={.complete=true};uint32_t returns=0;
    EffectBodyRecordCursor cursor;
    XrXirStatus sequence=effect_body_request_sequence_begin(&flow->body,&cursor);
    if (sequence!=XR_XIR_OK) return sequence;
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        XrXirInstruction body_op_1;const XrXirInstruction *op=NULL;
        XrXirStatus body_read_1=effect_body_request_sequence_instruction(&flow->body,
            flow->read->final->nodes[flow->node].body,i,&cursor,&body_op_1,&op);
        if (body_read_1!=XR_XIR_OK) return body_read_1;
        if (op->op!=XR_XIR_RETURN) continue;
        ++returns;XrXirStatus status=effect_invocation_class_source(flow,op->args[0],&sources);
        if (status!=XR_XIR_OK) return status;
    }
    sequence=effect_body_request_sequence_finish(&flow->body,flow->read->final->nodes[flow->node].body,&cursor);
    if (sequence!=XR_XIR_OK) return sequence;
    if (!returns) sources.mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_UNKNOWN;
    EffectInvocationClassFact returned={.mask=node->class_return_mask,.rank=node->class_return_rank};
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
    EffectBodyFunctionHead body_head={0},root_head={0};
    if (!xir_compile_work(owner->work,sizeof(*request))) return XR_XIR_BUDGET;
    EffectInvocationFlow flow={.read=owner,.node=selected,.body=*request};
    XrXirStatus status=effect_body_request_head(request,function,&body_head);
    if (status==XR_XIR_OK) status=effect_body_request_head(request,node->root,&root_head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionHead *body=&body_head;
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

static XrXirStatus effect_invocation_class_verify_read(const EffectInvocationRead *owner,
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
    EffectCellReplayRows rows={0};uint64_t limit=0;
    XrXirStatus status=effect_invocation_class_cells_begin(owner,request,&rows);
    if (status==XR_XIR_OK)
        status=effect_invocation_class_rank_limit_count(owner,request->domain.function_count,&limit);
    for (uint32_t f=0;status==XR_XIR_OK && f<request->domain.function_count;++f)
        status=effect_invocation_class_root_match(owner,request,f);
    for (uint32_t n=0;status==XR_XIR_OK && n<owner->final->count;++n) {
        if (!xir_compile_work(owner->work,4)) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationNode *node=&owner->final->nodes[n];
        if (node->body>=request->domain.function_count ||
            node->class_count!=owner->final->physical->functions[node->body].classes) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        EffectInvocationFlow flow={.read=owner,.node=n,.instruction=UINT32_MAX,.precision=true};
        status=effect_invocation_origins_read(&flow,request);
        if (status==XR_XIR_OK) status=effect_invocation_class_body_match(&flow,limit,rows.functions?&rows.functions[node->body]:NULL);
        if (status==XR_XIR_OK) status=effect_invocation_class_terminals_match(&flow);
        effect_invocation_flow_finish(&flow);
    }
    effect_cell_replay_rows_free(&rows);return status;
}

static XrXirStatus effect_invocation_class_verify(EffectInvocationWorkspace *owner) {
    if (!owner || owner->pending || !effect_body_material_shape(owner->final.material) || !owner->read.work) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(owner->read.work,owner->final.material);
    return effect_invocation_class_verify_read(&owner->read,&request);
}
