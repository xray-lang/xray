/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_control_verify.inc.c - Owned control facts and root-entry shape
 */
static XrXirStatus effect_control_entry(
    const EffectBodyRequest *input,const EffectFinalOutcomes *final,uint32_t f) {
    EffectBodyFunctionHead head={0};XrXirStatus status=effect_body_request_head(input,f,&head);
    if (status!=XR_XIR_OK) return status;
    const XrXirFunctionEffectContract *contract=&final->contracts[f];
    const XrXirRootFormula *formula=&contract->formula;const XirEffectEntryRootView *entry=&final->entry_roots[f];
    uint32_t mask=formula->constant_mask,at=0;
    if (contract->parameter_count!=head.parameter_count || !!contract->parameters!=!!contract->parameter_count ||
        !!formula->terms!=!!formula->term_count || !!entry->parameters!=!!entry->parameter_count ||
        (mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED))) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t t=0;t<formula->term_count;++t) {

        const XrXirRootTerm *term=&formula->terms[t];
        if (term->kind==XR_XIR_ROOT_TERM_CONTEXT_CALL) mask|=XR_XIR_CALLABLE_ROOT_UNRESOLVED;
        else {
            if (term->index>=head.parameter_count) return XR_XIR_BAD_STRUCTURE;
            if (term->kind==XR_XIR_ROOT_TERM_CELL_PARAMETER) {
                if (!xr_xir_type_is_cell(input->domain.types,head.parameters[term->index]) ||
                    at>=entry->parameter_count || (at && entry->parameters[at-1]>=term->index) ||
                    entry->parameters[at++]!=term->index) return XR_XIR_BAD_STRUCTURE;
            } else if (term->kind==XR_XIR_ROOT_TERM_PARAMETER) {
                const XrXirTypeNode *signature=xr_xir_callable_signature(input->domain.types,head.parameters[term->index]);
                if (!signature || contract->parameters[term->index].kind!=XR_XIR_EFFECT_PARAMETER_VARIABLE)
                    return XR_XIR_BAD_STRUCTURE;
                mask|=signature->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED);
            } else return XR_XIR_BAD_STRUCTURE;
        }
    }
    return mask==entry->intrinsic_mask && at==entry->parameter_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_final_control_verify(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectFinalOutcomes *final = effect_final_outcomes(effects);
    if (!final || !xir_effects_context_matches(work, effects, final->count))
        return XR_XIR_BAD_STRUCTURE;
    const EffectControlSummary *summary = final->control_summary;
    XrXirStatus status = effect_role_range(work, summary, 1, sizeof(*summary));
    if (status != XR_XIR_OK) return status;
    if (summary->resources != work->resources || summary->functions != final->count ||
        summary->facts != final->functions || summary->causes != final->witnesses ||
        summary->task_creation != final->task_creation) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest input = effect_body_owned_request(work, effects->invocations->body);
    for (uint32_t f = 0; f < final->count; ++f) {
        status = effect_control_entry(&input, final, f);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
