/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_capture.inc.c - Authentic latent capture bound correspondence
 *
 * KEY CONCEPT:
 *   A closed target address is not precision. Only original variable captures
 *   and the same derived formula authenticate a different latent outer bound.
 */
static XrXirStatus effect_capture_identity(const XrXirCompileContext *context,
    const XirEffectCaptureRequest *request, bool *authentic) {
    *authentic=false;
    const XrXirModule *source=request->source;
    const XrXirProvenance *p=source->provenance;
    if (!p || p->kind!=XR_XIR_EVIDENCE_TEMPLATE || !p->contracts ||
        p->contract_count!=source->function_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionEffectContract *contract=&p->contracts[request->caller->function];
    if (!!contract->value_count!=!!contract->values) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *op=&source->functions[request->caller->function].instructions[request->instruction];
    for (uint32_t v=0;v<contract->value_count;++v) {
        if (!xir_compile_work(context,3)) return XR_XIR_BUDGET;
        const XrXirRootValueIdentity *identity=&contract->values[v];
        if (identity->instruction!=request->instruction) continue;
        if (identity->mode!=XR_XIR_EFFECT_VALUE_AUTHENTIC_REF || identity->declared_type!=op->type)
            return XR_XIR_BAD_TYPE;
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        *authentic=true;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effect_capture_bound(const XrXirCompileContext *context,
    const XirEffectCaptureRequest *q, XirEffectCaptureBound *output) {
    if (!xir_compile_context_valid(context) || !q || !output || !q->source || !q->source->functions ||
        !q->source->types || !q->types || !q->actual || !q->effects || !q->caller || !q->target ||
        !xir_effects_context_matches(context,q->effects,q->source->function_count))
        return XR_XIR_BAD_STRUCTURE;
    if (q->caller->function>=q->source->function_count || q->target->function>=q->source->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,12)) return XR_XIR_BUDGET;
    const XrXirFunction *original=&q->source->functions[q->caller->function];
    if (!original->instructions || !q->actual->instructions || q->instruction>=original->instruction_count ||
        q->instruction>=q->actual->instruction_count ||
        original->parameter_count!=q->actual->parameter_count ||
        original->instruction_count!=q->actual->instruction_count ||
        !!q->caller->argument_count!=!!q->caller->arguments ||
        !!q->target->argument_count!=!!q->target->arguments ||
        !!q->target->effect_argument_count!=!!q->target->effect_arguments) return XR_XIR_BAD_STRUCTURE;
    const XrXirInstruction *before=&original->instructions[q->instruction];
    const XrXirInstruction *after=&q->actual->instructions[q->instruction];
    if (before->op!=XR_XIR_FUNCTION_REF || after->op!=XR_XIR_FUNCTION_REF || before->immediate<0 ||
        (uint64_t)before->immediate!=q->target->function || before->args[0]!=after->args[0] ||
        before->args[1]!=after->args[1] || (after->args[1] && !q->actual->operands) ||
        after->args[0]>q->actual->operand_count ||
        after->args[1]>q->actual->operand_count-after->args[0]) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypeNode *signature=xr_xir_callable_signature(q->source->types,before->type);
    const XrXirFunctionEffectContract *contract=xir_effects_contract(q->effects,q->target->function);
    if (!signature || !contract || contract->parameter_count!=q->source->functions[q->target->function].parameter_count ||
        !!contract->parameter_count!=!!contract->parameters || after->args[1]>contract->parameter_count ||
        q->target->effect_argument_count>contract->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t caller_count=q->source->generics?q->source->generics[q->caller->function].parameter_count:0;
    uint32_t target_count=q->source->generics?q->source->generics[q->target->function].parameter_count:0;
    if (q->caller->argument_count!=caller_count || q->target->argument_count!=target_count)
        return XR_XIR_BAD_STRUCTURE;
    XirEffectCallableBound bound={q->source->types,q->types,q->caller->arguments,
        q->caller->argument_count,before->type,after->type};
    XrXirStatus status=xir_effect_callable_bound_matches(context,&bound);
    bool authentic=false;
    if (status==XR_XIR_OK) status=effect_capture_identity(context,q,&authentic);
    XirEffectCaptureBound result={signature->flags,false};
    if (status!=XR_XIR_OK) return status;
    if (!authentic) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        *output=result;return XR_XIR_OK;
    }
    XirEffectContextInput input={q->source,q->types,q->caller,q->instruction,q->owners,q->owner_count};
    XirEffectContextView selected={0};
    status=xir_effects_context_select(context,q->effects,&input,&selected);
    if (status!=XR_XIR_OK) return status;
    if (selected.declaration!=q->target->function || selected.parameter_count!=q->target->effect_argument_count ||
        selected.argument_count!=q->target->argument_count) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    for (uint32_t a=0;a<selected.argument_count && status==XR_XIR_OK;++a)
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            selected.types,q->types,NULL,0,selected.arguments[a],q->target->arguments[a],&scratch);
    for (uint32_t p=0;p<selected.parameter_count && status==XR_XIR_OK;++p) {
        if (q->target->effect_arguments[p].parameter!=p) { status=XR_XIR_BAD_STRUCTURE;break; }
        status=xr_xir_compile_type_substitution_matches_between_scratch(context,
            selected.types,q->types,NULL,0,selected.physical_types[p],q->target->effect_arguments[p].type,&scratch);
    }
    xr_xir_type_match_scratch_free(&scratch);
    if (status!=XR_XIR_OK) return status;
    uint32_t mask=(selected.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
        (selected.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
    result.flags=(mask?mask:XR_XIR_CALLABLE_ROOT_NONE)|(signature->flags&XR_XIR_CALLABLE_NO_SUSPEND);
    if (!xr_xir_callable_flags_compatible(result.flags,signature->flags)) return XR_XIR_BAD_TYPE;
    result.refined=result.flags!=signature->flags;
    if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
    *output=result;
    return XR_XIR_OK;
}
