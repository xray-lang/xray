/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_program_receive.inc.c - Current owned Input and Final reception
 */
#include "xxir_layout_input_internal.h"
#include "xxir_effect_program_final_storage.inc.c"
#include "xxir_effect_program_outcome_current.inc.c"
#include "xxir_effect_program_error_leaf_current.inc.c"

static XrXirStatus effect_program_layout_head(const XrXirCompileContext *work,
    const void *owner,uint32_t function,XirInputFunctionHead *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRequest request=effect_body_owned_request(work,owner);
    EffectBodyFunctionHead head;
    XrXirStatus status=request.head(&request,function,&head,NULL);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XirInputFunctionHead){head.name,head.name_length,head.parameters,head.parameter_count,
        head.result,head.blocks,head.block_count,head.instruction_count,head.operands,head.operand_count};
    return XR_XIR_OK;
}
static XrXirStatus effect_program_layout_instruction(const XrXirCompileContext *work,
    const void *owner,uint32_t function,uint32_t instruction,
    XrXirInstruction *storage,const XrXirInstruction **output) {
    EffectBodyRequest request=effect_body_owned_request(work,owner);
    return request.instruction(&request,function,instruction,storage,output,NULL);
}
XR_FUNC XrXirStatus xir_effects_program_layout_match(const XrXirCompileContext *work,
    const XrXirEffects *effects,const XrXirTarget *target,XrXirFunctionLayout *layouts) {
    if (!xir_compile_context_valid(work) || !effects || !target) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyMaterial *body=NULL;
    XrXirStatus status=effect_program_input_storage(work,effects,&body);
    if (status!=XR_XIR_OK) return status;
    XirLayoutInput input={body->domain,body,effect_program_layout_head,effect_program_layout_instruction};
    return xir_layout_input_match(work,&input,target,layouts);
}

/* Source templates have authentic specialization contexts; closed execution
 * inputs have no such owner. Both cases replay the same current certificate
 * and independent Control/Error algorithms, never a saved constructor result. */
static XrXirStatus effect_program_parameter_domains(const XrXirCompileContext *work,
    const XrXirEffects *effects) {
    const EffectBodyMaterial *body=effects->invocations->body;
    const XrXirFunctionEffectContract *contracts=effect_contracts_read(effects);
    for(uint32_t f=0;f<effects->count;++f) {
        XrXirStatus status=effect_body_initial_roles_match(work,body,f);
        if(status!=XR_XIR_OK) return status;
        const EffectBodyFunction *function=&body->functions[f];
        for(uint32_t p=0;p<function->parameter_count;++p) {
            if(!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            const XrXirEffectParameter *parameter=&contracts[f].parameters[p];
            if(parameter->kind>XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC) return XR_XIR_BAD_STRUCTURE;
            /* Fixed domains contain no callable-origin variable or use row. */
            bool fixed=function->initial_roles[p]==XR_XIR_EFFECT_PARAMETER_FIXED ||
                !xr_xir_callable_signature(body->domain.types,function->parameters[p]);
            if(fixed && (parameter->kind!=XR_XIR_EFFECT_PARAMETER_FIXED || parameter->uses))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_effects_program_final_verify(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    XrXirStatus status=xir_effects_program_body_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_program_final_storage(work,effects);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectFinalOutcomes *final=effect_final_outcomes(effects);
    if (!final || effects->generation || effects->refinement) return XR_XIR_BAD_STRUCTURE;
    status=effect_source_certificate(work,certificate,effects);
    if(status==XR_XIR_OK) status=effect_program_parameter_domains(work,effects);
    bool contexts=source->provenance && source->provenance->kind==XR_XIR_EVIDENCE_TEMPLATE &&
        source->declarations && source->declarations->functions;
    if (status==XR_XIR_OK) {
        if (contexts) status=effect_source_contexts(work,source,effects);
        else if (final->contexts) status=XR_XIR_BAD_STRUCTURE;
    }
    if (status==XR_XIR_OK) status=effect_source_publication_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_source_outcomes(work,effects);
    if (status==XR_XIR_OK) status=xir_effects_program_body_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_invocation_current_input_match(work,source,certificate->body);
    return status;
}

/* No allocation or release occurs in this current ROOT replay. The complete
 * receiving proof still precedes it; this closes later preparation callbacks
 * over the actual equations, causes, formulas and published ROOT records. */
XR_FUNC XrXirStatus xir_effects_program_root_current(const XrXirCompileContext *work,
    const XrXirModule *source,const XrXirEffects *effects) {
    XrXirStatus status=xir_effects_program_body_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_program_final_storage(work,effects);
    if (status!=XR_XIR_OK) return status;
    const EffectInvocationCertificate *certificate=effects->invocations;
    status=effect_source_equation_shape(work,certificate);
    if (status==XR_XIR_OK) status=effect_source_result_views(work,certificate,effects);
    if (status==XR_XIR_OK) status=effect_program_parameter_domains(work,effects);
    if (status==XR_XIR_OK) status=effect_source_atoms(work,certificate);
    if (status==XR_XIR_OK) status=effect_source_projections(work,certificate);
    if (status==XR_XIR_OK) status=effect_source_publication_match(work,source,effects);
    if (status==XR_XIR_OK) status=effect_program_suspension_current(work,effects);
    bool empty=false;
    if (status==XR_XIR_OK) status=effect_error_empty_current(work,effects,&empty);
    if (status==XR_XIR_OK) status=effect_program_error_projection(work,effects);
    if (status==XR_XIR_OK && !empty) status=effect_program_error_leaves_current(work,effects);
    return status;
}

/* The structural checker derives the same Cell proof through the sole owned
 * Input reader. The raw Lowered projection is matched again after callbacks. */
XR_FUNC XrXirStatus xir_effects_program_cells_match(const XrXirCompileContext *work,
    const XrXirEffects *effects,XrXirDiagnostic *diagnostic) {
    const EffectBodyMaterial *body=NULL;
    XrXirStatus status=effect_program_input_storage(work,effects,&body);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    if (status==XR_XIR_OK) status=effect_cell_body_owned(work,body,&adapter,&read);
    if (status==XR_XIR_OK)
        status=xr_xir_compile_cell_provenance_body_matches(work,body->cells,&read,diagnostic);
    return status;
}
