/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_dense.inc.c - Complete physical type vectors
 *
 * KEY CONCEPT:
 *   Every physical position has an identity. Only freshly classified variable
 *   callable arguments retain precision; other arguments keep ordinary exact
 *   types or the original complete callable bound.
 */
/* This internal view is consumed only by its execution owner. Its seeded
 * ordinary descriptor prefix keeps identical IDs and is never rewritten;
 * all later structural descriptors are append-only in the same type pool. */
typedef struct EffectDenseRequest {
    const EffectOrdinaryContexts *contexts;
    EffectOrdinaryContexts *execution;
    uint32_t selected, caller_index;
    const XrXirTypes *actual_types;
    const XrXirFunction *caller;
    const XrXirInstruction *instruction;
    const uint8_t *fixed;
    uint32_t value_count;
} EffectDenseRequest;

/* Stack view; its complete physical array is owned by execution->terms. */
typedef struct EffectDenseVector {
    const EffectTerms *terms;
    const XrXirType *physical;
    uint32_t count;
} EffectDenseVector;

typedef struct EffectDenseActual {
    XrXirType type;
    bool fixed, present;
} EffectDenseActual;

static XrXirStatus effect_dense_actual(const EffectDenseRequest *request,
    uint32_t parameter, EffectDenseActual *actual) {
    actual->present=parameter<request->instruction->args[1];
    actual->fixed=false;
    if (!actual->present) return XR_XIR_OK;
    if (!xir_compile_work(request->execution->terms.remaining,2)) return XR_XIR_BUDGET;
    uint32_t value=request->caller->operands[request->instruction->args[0]+parameter];
    if (value>=request->value_count) return XR_XIR_BAD_VALUE;
    actual->fixed=request->fixed[value]!=0;
    actual->type=xr_xir_operand_type(request->caller,value);
    return XR_XIR_OK;
}

static XrXirStatus effect_dense_parameter(const EffectDenseRequest *request,
    uint32_t parameter, XrXirType *output) {
    EffectTerms *terms=&request->execution->terms;
    const XrXirFunction *target=&request->contexts->functions[request->selected];
    XrXirType declared=target->parameters[parameter];
    EffectDenseActual actual={declared,false,false};
    XrXirStatus status=effect_dense_actual(request,parameter,&actual);
    if (status!=XR_XIR_OK) return status;
    const XrXirTypeNode *callable=xr_xir_callable_signature(&terms->types,declared);
    if (actual.present && callable) {
        XirEffectCallableBound bound={&terms->types,&terms->types,NULL,0,declared,actual.type};
        status=effect_callable_bound_matches_scratch(terms->remaining,&bound,effect_terms_type_scratch(terms));
    } else if (actual.present) {
        XrXirTypeMatchScratch *scratch=effect_terms_type_scratch(terms);
        status=xr_xir_compile_type_substitution_matches_between_scratch(terms->remaining,
            &terms->types,&terms->types,NULL,0,declared,actual.type,scratch);
    }
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(terms->remaining,3+sizeof(*output))) return XR_XIR_BUDGET;
    uint32_t kind=request->contexts->uses.contracts[request->selected].parameters[parameter].kind;
    *output=callable && actual.present && !actual.fixed && kind==XR_XIR_EFFECT_PARAMETER_VARIABLE ?
        actual.type : declared;
    return XR_XIR_OK;
}

/* The owner construction deep-copies and completely checks the ordinary
 * declaration domain once. No foreign pool is admitted by this edge helper.
 * Missing capture suffixes keep original bounds; unknown never becomes NONE. */
static XrXirStatus effect_dense_build(const XrXirCompileContext *context,
    const EffectDenseRequest *request, EffectDenseVector *output) {
    if (!xir_compile_context_valid(context) || !request || !request->contexts ||
        !request->execution || !output || output->terms || output->physical || output->count ||
        request->contexts->uses.resources!=context->resources ||
        !request->execution->dense || !request->execution->terms.remaining ||
        request->execution->terms.remaining->resources!=context->resources ||
        request->execution->terms.status!=XR_XIR_OK ||
        request->execution->terms.types.count<request->contexts->terms.types.count ||
        request->selected>=request->contexts->count || !request->contexts->uses.contracts ||
        request->caller_index>=request->execution->count ||
        request->caller!=&request->execution->functions[request->caller_index] ||
        request->actual_types!=&request->execution->terms.types || !request->instruction ||
        (!!request->value_count && !request->fixed)) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *target=&request->contexts->functions[request->selected];
    const XrXirFunction *caller=request->caller;
    const XrXirInstruction *op=request->instruction;
    if (request->contexts->uses.contracts[request->selected].parameter_count!=target->parameter_count ||
        (uint64_t)caller->parameter_count+caller->instruction_count!=request->value_count ||
        (op->args[1] && !caller->operands) || op->args[0]>caller->operand_count ||
        op->args[1]>caller->operand_count-op->args[0]) return XR_XIR_BAD_STRUCTURE;
    uint32_t family=effect_context_family(op->op);
    if (!family || op->args[1]>target->parameter_count ||
        (family!=XR_XIR_EFFECT_BINDING_CAPTURE && op->args[1]!=target->parameter_count))
        return XR_XIR_BAD_STRUCTURE;
    EffectTerms *terms=&request->execution->terms;
    XrXirType *physical=target->parameter_count ?
        effect_terms_alloc(terms,target->parameter_count,sizeof(*physical)) : NULL;
    if (target->parameter_count && !physical) return terms->status;
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t p=0;p<target->parameter_count && status==XR_XIR_OK;++p)
        status=effect_dense_parameter(request,p,&physical[p]);
    if (status!=XR_XIR_OK) return status;
    *output=(EffectDenseVector){terms,physical,target->parameter_count};return XR_XIR_OK;
}
