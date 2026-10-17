/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_control_verify.inc.c - Independent receiving proof of control facts
 *
 * KEY CONCEPT:
 *   Input supplies every edge and terminal. Final supplies the proposed facts.
 *   Final traversal positions and actual incoming instruction IDs prove
 *   leastness without receiving scratch. Error flow has its separate proof.
 */
typedef struct EffectControlPositions { uint32_t position[3]; } EffectControlPositions;
typedef struct EffectControlEdgeId { uint32_t caller,instruction; } EffectControlEdgeId;
struct EffectControlProof {
    XrCompileResources *resources;
    const EffectBodyMaterial *material;
    uint32_t functions,counts[3],edges;
};
static bool effect_control_cause_is(const XrXirEffectWitness *actual,
    XrXirEffectCause cause,uint32_t instruction,uint32_t callee,uint32_t distance) {
    return actual->cause==cause && actual->instruction==instruction &&
        actual->callee==callee && actual->distance==distance;
}

static XrXirStatus effect_control_target(const EffectBodyRequest *input,uint32_t count,const XrXirInstruction *op,
    bool *direct,uint32_t *target,bool *cleanup) {
    *direct=false;*cleanup=false;*target=UINT32_MAX;
    if (op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_CALL_DEFAULT &&
        op->op!=XR_XIR_INVOKE_DEFAULT && op->op!=XR_XIR_CLEANUP_REGISTER) return XR_XIR_OK;
    *direct=true;*cleanup=op->op==XR_XIR_CLEANUP_REGISTER;
    if (op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_INVOKE_DEFAULT) {
        const uint32_t *identity=xr_xir_default_identity(op);const XrXirDefaultBinding *binding=NULL;
        XrXirStatus status=xir_metadata_default_lookup(input->read.owner,&input->domain,
            identity[0],identity[1],&binding);
        if (status!=XR_XIR_OK) return status;
        if (!binding) return XR_XIR_BAD_STRUCTURE;
        *target=binding->function;
    } else {
        if (op->immediate<0 || (uint64_t)op->immediate>=count) return XR_XIR_BAD_STRUCTURE;
        *target=(uint32_t)op->immediate;
    }
    return *target<count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_control_entry(const XrXirCompileContext *work,
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
        if (!xir_compile_work(work,6)) return XR_XIR_BUDGET;
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

#include "xxir_effect_control_proof.inc.c"

static inline XrXirStatus effect_final_control_verify(const XrXirCompileContext *work,const XrXirEffects *effects) {
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if(!final || !xir_effects_context_matches(work,effects,final->count)) return XR_XIR_BAD_STRUCTURE;
    return effect_control_proof_check(work,effects->invocations,final);
}
