/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_class_cells.inc.c - Complete Class subjects in Cell terminals
 *
 * The same provider proves each true place subject. Only the exact equation's
 * complete masks and descending ranks substitute its conservative Class bit.
 */
static XrXirStatus effect_invocation_class_cells_match(EffectInvocationWorkspace *owner,
    const EffectBodyRequest *body) {
    if (!owner || !body || !owner->work || !owner->effects || !owner->final.cells ||
        !xir_effects_context_matches(owner->work,owner->effects,body->domain.function_count))
        return XR_XIR_BAD_STRUCTURE;
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    XrXirStatus status=effect_cell_body_request(owner->work,body,&adapter,&read);
    if (status!=XR_XIR_OK) return status;
    return xr_xir_compile_cell_provenance_body_matches(owner->work,owner->final.cells,&read,NULL);
}

static XrXirStatus effect_invocation_class_cell_subject(EffectInvocationFlow *flow,
    const XrXirCellClassRoot *root,uint32_t *mask) {
    EffectInvocationWorkspace *owner=flow->owner;
    if (!root || !mask || root->place<flow->function->parameter_count ||
        root->place>=flow->values || root->value>=flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage={0};const XrXirInstruction *place=NULL;
    XrXirStatus status=effect_invocation_flow_instruction(flow,
        root->place-flow->function->parameter_count,&storage,&place);
    XrXirType subject=XR_XIR_UNIT;
    if (status==XR_XIR_OK) status=effect_invocation_flow_type(flow,root->value,&subject);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
    if (!place || place->op!=XR_XIR_OBJECT_PLACE || place->args[0]!=root->value ||
        place->type!=subject || !xr_xir_type_is_class(flow->body.domain.types,subject))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=0;
    status=effect_invocation_class_find(owner->work,&owner->final.nodes[flow->node],root->value,&selected);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    const EffectInvocationClassFact *fact=&owner->final.nodes[flow->node].classes[selected];
    if (!fact->mask) return XR_XIR_UNRESOLVED;
    if ((fact->mask&~EFFECT_CLASS_VALID) || !(fact->mask&EFFECT_CLASS_KNOWN) ||
        (fact->mask==EFFECT_CLASS_KNOWN ? !fact->rank : fact->rank!=0))
        return XR_XIR_BAD_STRUCTURE;
    *mask=fact->mask;return XR_XIR_OK;
}

/* A bottom delays this real terminal on the one equation queue. It is not an
 * UNKNOWN seed and cannot certify None. Quiet residual filling stays intact. */
static XrXirStatus effect_invocation_class_cell_view(EffectInvocationFlow *flow,
    uint32_t value,XrXirCellOriginView *output) {
    EffectInvocationWorkspace *owner=flow->owner;
    if (!output || !owner || !owner->work || !owner->effects || !owner->final.cells ||
        flow->node>=owner->final.count || value>=flow->values) return XR_XIR_BAD_STRUCTURE;
    XrXirCellClassOriginView view={0};
    XrXirStatus status=xr_xir_compile_cell_class_origin_view(owner->work,owner->final.cells,
        owner->final.nodes[flow->node].body,value,&view);
    if (status!=XR_XIR_OK) return status;
    uint32_t intrinsic=view.residual_intrinsic_mask;
    for (uint32_t w=0;w<view.word_count;++w) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        uint64_t bits=view.dependencies[w];
        while (bits) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint32_t bit=(uint32_t)__builtin_ctzll(bits);bits&=bits-1;
            uint64_t index=(uint64_t)w*64+bit;
            if (index>=view.root_count || !view.roots) return XR_XIR_BAD_STRUCTURE;
            uint32_t mask=0;
            status=effect_invocation_class_cell_subject(flow,&view.roots[(uint32_t)index],&mask);
            if (status!=XR_XIR_OK) return status;
            if (mask&EFFECT_CLASS_ROOT) intrinsic|=XR_XIR_CELL_ACCESS_ROOT;
            if (mask&EFFECT_CLASS_UNKNOWN) intrinsic|=XR_XIR_CELL_ACCESS_UNKNOWN;
        }
    }
    if (!xir_compile_work(owner->work,sizeof(*output))) return XR_XIR_BUDGET;
    view.cells.intrinsic_mask=intrinsic;*output=view.cells;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_cell(EffectInvocationFlow *flow,
    uint32_t value,uint64_t *output,bool *changed);

/* The original terminal and final correspondence use this one mask decoder.
 * It reads the complete actual row, including its explicit known bit. */
static XrXirStatus effect_invocation_cell_mask(EffectInvocationFlow *flow,
    const uint64_t *row,uint32_t *output) {
    if (!flow || !row || !output) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(flow->owner->work,4)) return XR_XIR_BUDGET;
    uint32_t known=flow->basis.parameters+2,mask=0;
    if (!(row[known/64]&(UINT64_C(1)<<(known%64))))
        mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    if (row[flow->basis.parameters/64]&(UINT64_C(1)<<(flow->basis.parameters%64)))
        mask|=XR_XIR_CALLABLE_ROOT_REQUIRED;
    uint32_t unknown=flow->basis.parameters+1;
    if (row[unknown/64]&(UINT64_C(1)<<(unknown%64))) mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
    *output=mask;return XR_XIR_OK;
}

static bool effect_invocation_cell_terminal(XrXirOp op) {
    return op==XR_XIR_CELL_READ || op==XR_XIR_CELL_WRITE || op==XR_XIR_CELL_LOCAL_WRITE ||
        op==XR_XIR_PLACE_READ || op==XR_XIR_PLACE_WRITE || op==XR_XIR_CELL_PROJECT;
}

/* Each real terminal contribution must be represented locally. A local Cell
 * witness must in turn be supported by that exact final contribution, and
 * cannot replace an earlier real terminal with a later or forged witness. */
static XrXirStatus effect_invocation_class_terminal_cause(EffectInvocationFlow *flow,
    uint32_t atom,XrXirRootEffectWitness witness,uint64_t *confirmed) {
    EffectInvocationNode *node=&flow->owner->final.nodes[flow->node];
    if (!confirmed || atom>=node->cause_count || !effect_invocation_atom(node,
        flow->basis.parameters,atom,true)) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationCause *local=&effect_invocation_local_causes(node)[atom];
    if (!xir_compile_work(flow->owner->work,6)) return XR_XIR_BUDGET;
    if (!local->witness.cause || local->distance || local->witness.distance ||
        local->next!=UINT32_MAX || local->edge!=UINT32_MAX ||
        effect_root_cause_before(&witness,&local->witness)) return XR_XIR_BAD_STRUCTURE;
    if (local->witness.cause!=witness.cause || local->witness.instruction!=witness.instruction ||
        local->witness.callee!=witness.callee || local->witness.slot!=witness.slot)
        return XR_XIR_OK;
    uint32_t bit=atom<2 ? atom : atom-flow->basis.parameters;
    if (!xir_compile_work(flow->owner->work,2)) return XR_XIR_BUDGET;
    confirmed[bit/64]|=UINT64_C(1)<<(bit%64);return XR_XIR_OK;
}

static XrXirStatus effect_invocation_class_terminals_match(EffectInvocationFlow *flow) {
    EffectInvocationWorkspace *owner=flow->owner;
    if (!owner || flow->node>=owner->final.count || !flow->physical || !flow->basis.cell_words)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final.nodes[flow->node];
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    if (!effect_invocation_node_shape(node,flow->basis.parameters,
        flow->function->instruction_count)) return XR_XIR_BAD_STRUCTURE;
    uint64_t bytes=(uint64_t)flow->basis.cell_words*sizeof(uint64_t);
    if (bytes>SIZE_MAX/2) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint64_t *storage=xir_compile_calloc(owner->work,2,(size_t)bytes,&status);
    if (!storage) return status;
    uint64_t *row=storage,*confirmed=storage+flow->basis.cell_words;
    EffectBodyFunctionHead root={0};
    status=effect_invocation_flow_head(flow,node->root,&root);
    for (uint32_t r=0;status==XR_XIR_OK && r<flow->physical->count;++r) {
        if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
        EffectPhysicalRecord record=flow->physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_INVOCATION)) continue;
        if (record.value<flow->function->parameter_count || record.value>=flow->values) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        uint32_t i=record.value-flow->function->parameter_count;
        XrXirInstruction op_storage={0};const XrXirInstruction *op=NULL;
        status=effect_invocation_flow_instruction(flow,i,&op_storage,&op);
        if (status!=XR_XIR_OK) break;
        if (!effect_invocation_cell_terminal(op->op)) continue;
        if (!xir_compile_work(owner->work,bytes)) { status=XR_XIR_BUDGET;break; }
        memset(row,0,(size_t)bytes);flow->instruction=i;
        status=effect_invocation_cell(flow,op->args[0],row,NULL);
        if (status==XR_XIR_UNRESOLVED) status=XR_XIR_BAD_STRUCTURE;
        uint32_t mask=0;
        if (status==XR_XIR_OK) status=effect_invocation_cell_mask(flow,row,&mask);
        if (status!=XR_XIR_OK) break;
        if (mask&~node->local_mask) { status=XR_XIR_BAD_STRUCTURE;break; }
        for (uint32_t atom=0;atom<2 && status==XR_XIR_OK;++atom) {
            uint32_t flag=atom?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
            if (!(mask&flag)) continue;
            XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CELL_ACCESS,i,UINT32_MAX,op->args[0],0};
            status=effect_invocation_class_terminal_cause(flow,atom,witness,confirmed);
        }
        for (uint32_t p=0;p<flow->basis.parameters && status==XR_XIR_OK;++p) {
            if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
            if (!(row[p/64]&(UINT64_C(1)<<(p%64)))) continue;
            if (p>=root.parameter_count || !root.parameters ||
                !xr_xir_type_is_cell(flow->body.domain.types,root.parameters[p])) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            XrXirRootEffectWitness witness={XR_XIR_ROOT_CAUSE_CELL_PARAMETER,i,UINT32_MAX,p,0};
            status=effect_invocation_class_terminal_cause(flow,flow->basis.parameters+p+2,witness,confirmed);
        }
    }
    for (uint32_t bit=0;status==XR_XIR_OK && bit<flow->basis.parameters+2;++bit) {
        if (!xir_compile_work(owner->work,3)) { status=XR_XIR_BUDGET;break; }
        uint32_t atom=bit<2?bit:flow->basis.parameters+bit;
        const EffectInvocationCause *local=&effect_invocation_local_causes(node)[atom];
        if (local->witness.cause!=XR_XIR_ROOT_CAUSE_CELL_ACCESS &&
            local->witness.cause!=XR_XIR_ROOT_CAUSE_CELL_PARAMETER) continue;
        if (!(confirmed[bit/64]&(UINT64_C(1)<<(bit%64)))) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(storage);return status;
}

/* Only the actual terminal may add to the immutable local cause copy during
 * precision. Child joins cannot be copied back and called local evidence. */
static XrXirStatus effect_invocation_cell_local(EffectInvocationFlow *flow,
    uint32_t atom,XrXirRootEffectWitness witness,bool *changed) {
    EffectInvocationWorkspace *owner=flow->owner;
    if (!flow->precision) return effect_invocation_local(owner,flow->node,atom,witness);
    if (!changed || flow->node>=owner->final.count || witness.distance ||
        witness.instruction!=flow->instruction || witness.callee!=UINT32_MAX ||
        flow->instruction>=flow->function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    EffectInvocationNode *node=&owner->final.nodes[flow->node];
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    if (!owner->states || !owner->states[flow->node].expanded || atom>=node->cause_count ||
        !effect_invocation_node_shape(node,flow->basis.parameters,flow->function->instruction_count))
        return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction storage={0};const XrXirInstruction *op=NULL;
    XrXirStatus status=effect_invocation_flow_instruction(flow,flow->instruction,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (!op || !effect_invocation_cell_terminal(op->op)) return XR_XIR_BAD_STRUCTURE;
    uint32_t p=atom>=flow->basis.parameters+2 ? atom-flow->basis.parameters-2 : UINT32_MAX;
    if (atom<2 ? witness.cause!=XR_XIR_ROOT_CAUSE_CELL_ACCESS || witness.slot!=op->args[0] :
        p>=flow->basis.parameters || witness.cause!=XR_XIR_ROOT_CAUSE_CELL_PARAMETER || witness.slot!=p)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,8+2*sizeof(EffectInvocationCause))) return XR_XIR_BUDGET;
    if (atom<2) {
        uint32_t flag=atom?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_REQUIRED;
        if (!(node->local_mask&flag)) { node->local_mask|=flag;*changed=true; }
    } else {
        uint64_t bit=UINT64_C(1)<<(p%64);
        if (!(node->local_cells[p/64]&bit)) { node->local_cells[p/64]|=bit;*changed=true; }
    }
    EffectInvocationCause *local=&effect_invocation_local_causes(node)[atom],*global=&node->causes[atom];
    EffectInvocationCause actual={witness,UINT32_MAX,UINT32_MAX,0};
    if (!local->witness.cause || effect_root_cause_before(&witness,&local->witness)) *local=actual;
    if (!global->witness.cause || effect_root_cause_before(&witness,&global->witness)) *global=actual;
    return XR_XIR_OK;
}
