/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_deferred_verify.inc.c - Exact delayed frontier closure
 *
 * KEY CONCEPT:
 *   A delayed invocation owns the real formal and complete physical actuals.
 *   Replaying the common value-origin flow verifies this correspondence before
 *   seal; it neither solves another invocation graph nor publishes permission.
 */
static XrXirStatus effect_invocation_deferred_match(EffectInvocationFlow *flow,
    const EffectInvocationDeferred *record) {
    EffectInvocationOwner *owner=flow->owner;
    if (record->node!=flow->node || record->instruction>=flow->function->instruction_count ||
        record->parameter>=flow->basis.parameters || !!record->arguments!=!!record->count)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&flow->function->instructions[record->instruction];
    if ((op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT) ||
        op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,
        xr_xir_operand_type(flow->function,(uint32_t)op->immediate));
    bool ref=false;XrXirStatus status=effect_invocation_has_ref(flow,signature,&ref);
    if (status!=XR_XIR_OK) return status;
    if (!ref || record->count!=signature->parameter_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t bit=owner->site_count+record->parameter;
    const uint64_t *descriptor=flow->rows+(size_t)op->immediate*flow->basis.fn_words;
    if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
    if (!(descriptor[bit/64]&(UINT64_C(1)<<(bit%64)))) return XR_XIR_BAD_STRUCTURE;
    uint64_t *actual=NULL;flow->instruction=record->instruction;
    status=effect_invocation_deferred_arguments(flow,record->instruction,signature,&actual);
    uint64_t words=(uint64_t)record->count*flow->basis.row_words;
    if (status==XR_XIR_OK && words>SIZE_MAX/sizeof(*actual)) status=XR_XIR_BUDGET;
    for (size_t w=0;status==XR_XIR_OK && w<(size_t)words;++w) {
        if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
        if (record->arguments[w]!=actual[w]) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(actual);return status;
}

/* Every real symbolic REF callee has exactly one local record per root formal.
 * Known producer alternatives remain separate executing edges. Direct generic,
 * default and requirement conditions retain their existing opaque obligations. */
static XrXirStatus effect_invocation_deferred_frontiers(EffectInvocationFlow *flow,
    uint32_t *matched) {
    EffectInvocationOwner *owner=flow->owner;
    const EffectInvocationNode *node=&owner->nodes[flow->node];
    for (uint32_t i=0;i<flow->function->instruction_count;++i) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&flow->function->instructions[i];
        if (!(node->local_contexts[i/64]&(UINT64_C(1)<<(i%64))) ||
            (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT)) continue;
        if (op->immediate<0 || (uint64_t)op->immediate>=flow->values) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,
            xr_xir_operand_type(flow->function,(uint32_t)op->immediate));
        bool ref=false;XrXirStatus status=effect_invocation_has_ref(flow,signature,&ref);
        if (status!=XR_XIR_OK) return status;
        if (!ref) continue;
        const uint64_t *descriptor=flow->rows+(size_t)op->immediate*flow->basis.fn_words;
        for (uint32_t p=0;p<flow->basis.parameters;++p) {
            if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
            uint32_t bit=owner->site_count+p;
            if (!(descriptor[bit/64]&(UINT64_C(1)<<(bit%64)))) continue;
            uint32_t selected=UINT32_MAX;
            for (uint32_t d=0;d<owner->deferred_count;++d) {
                if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
                const EffectInvocationDeferred *record=&owner->deferred_calls[d];
                if (record->node!=flow->node || record->instruction!=i || record->parameter!=p) continue;
                if (selected!=UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
                selected=d;
            }
            if (selected==UINT32_MAX) return XR_XIR_BAD_STRUCTURE;
            status=effect_invocation_deferred_match(flow,&owner->deferred_calls[selected]);
            if (status!=XR_XIR_OK) return status;
            if (*matched==UINT32_MAX) return XR_XIR_BUDGET;
            ++*matched;
        }
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_deferred_needed(EffectInvocationOwner *owner,
    uint32_t n,bool *output) {
    const EffectInvocationNode *node=&owner->nodes[n];
    const XrXirFunction *function=&owner->module->functions[node->body];
    uint64_t values=(uint64_t)function->parameter_count+function->instruction_count;
    bool needed=false;EffectInvocationFlow flow={.owner=owner};
    for (uint32_t i=0;i<function->instruction_count && node->local_deferred;++i) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&function->instructions[i];
        if (!(node->local_contexts[i/64]&(UINT64_C(1)<<(i%64))) ||
            (op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_INVOKE_INDIRECT)) continue;
        if (op->immediate<0 || (uint64_t)op->immediate>=values) return XR_XIR_BAD_STRUCTURE;
        const XrXirTypeNode *signature=xr_xir_callable_signature(owner->module->types,
            xr_xir_operand_type(function,(uint32_t)op->immediate));
        bool ref=false;XrXirStatus status=effect_invocation_has_ref(&flow,signature,&ref);
        if (status!=XR_XIR_OK) return status;
        needed|=ref;
    }
    *output=needed;return XR_XIR_OK;
}

/* The local SSA replay uses the same immutable input and common Cell proof.
 * All records, including missing and duplicate frontiers, are consumed before
 * any certificate ownership transfer. A failure leaves every producer owned. */
static XrXirStatus effect_invocation_deferred_verify(EffectInvocationOwner *owner) {
    if (!owner || !owner->module || !owner->effects || !owner->work || owner->pending ||
        owner->deferred_count>owner->deferred_capacity ||
        (owner->deferred_count && !owner->deferred_calls)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t d=0;d<owner->deferred_count;++d) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        const EffectInvocationDeferred *record=&owner->deferred_calls[d];
        if (record->node>=owner->count || !owner->nodes[record->node].local_deferred)
            return XR_XIR_BAD_STRUCTURE;
    }
    uint32_t matched=0;
    for (uint32_t n=0;n<owner->count;++n) {
        if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
        bool needed=false;XrXirStatus status=effect_invocation_deferred_needed(owner,n,&needed);
        if (status!=XR_XIR_OK) return status;
        if (!needed) continue;
        EffectInvocationFlow flow={.owner=owner,.node=n,.instruction=UINT32_MAX};
        status=effect_invocation_origins(&flow);
        if (status==XR_XIR_OK) status=effect_invocation_deferred_frontiers(&flow,&matched);
        xr_compile_resources_free(flow.memory);
        if (status!=XR_XIR_OK) return status;
    }
    return matched==owner->deferred_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

/* A staged packed formula is rechecked against the completed equation before
 * transfer. Exact storage/term order prevents a forged staged NONE or changed
 * term from becoming a canonical result. This does not run the call solver. */
static XrXirStatus effect_invocation_solution_match(EffectInvocationOwner *owner,
    const EffectInvocationSolution *solution) {
    if (!solution || solution->resources!=owner->work->resources ||
        solution->count!=owner->module->function_count || !solution->count ||
        !solution->storage || solution->formulas!=solution->storage) return XR_XIR_BAD_STRUCTURE;
    const XrXirRootTerm *records=(const XrXirRootTerm *)(solution->formulas+solution->count);
    XrXirRootTerm *expected=NULL;uint32_t capacity=0,at=0;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t f=0;f<solution->count && status==XR_XIR_OK;++f) {
        uint32_t count=0;status=effect_invocation_terms(owner,f,NULL,&count);
        if (status!=XR_XIR_OK) break;
        const XrXirRootFormula *formula=&solution->formulas[f];
        if (!xir_compile_work(owner->work,5)) { status=XR_XIR_BUDGET;break; }
        if (at>solution->term_count || count>solution->term_count-at ||
            formula->constant_mask!=owner->nodes[owner->roots[f]].intrinsic_mask ||
            formula->term_count!=count || formula->terms!=(count?records+at:NULL)) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (count>capacity) {
            if ((uint64_t)count>SIZE_MAX/sizeof(*expected)) { status=XR_XIR_BUDGET;break; }
            XrXirRootTerm *replacement=xir_compile_alloc(owner->work,(size_t)count*sizeof(*expected),&status);
            if (!replacement) break;
            xr_compile_resources_free(expected);expected=replacement;capacity=count;
        }
        uint32_t produced=0;status=effect_invocation_terms(owner,f,expected,&produced);
        if (status==XR_XIR_OK && produced!=count) status=XR_XIR_BAD_STRUCTURE;
        for (uint32_t t=0;status==XR_XIR_OK && t<count;++t) {
            if (!xir_compile_work(owner->work,4)) { status=XR_XIR_BUDGET;break; }
            if (formula->terms[t].kind!=expected[t].kind || formula->terms[t].index!=expected[t].index)
                status=XR_XIR_BAD_STRUCTURE;
        }
        at+=count;
    }
    xr_compile_resources_free(expected);
    if (status==XR_XIR_OK && at!=solution->term_count) status=XR_XIR_BAD_STRUCTURE;
    return status;
}
