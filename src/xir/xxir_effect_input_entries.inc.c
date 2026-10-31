/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_input_entries.inc.c - Complete current-input admission
 *
 * KEY CONCEPT:
 *   Raw current operations compare the actual construction before and after
 *   their synchronous implementation. No matching result grants a read right.
 */
XR_FUNC XrXirStatus xir_effects_source_body_match_input(const XrXirCompileContext *context, XirEffectInput input, const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_source_body_current(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_program_body_match_input(const XrXirCompileContext *context, XirEffectInput input, const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_program_body_current(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_program_final_verify_input(const XrXirCompileContext *context, XirEffectInput input, const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_program_input_final(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_source_terminal_input(const XrXirCompileContext *context, XirEffectInput input, const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_source_input_terminal(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_program_root_current_input(const XrXirCompileContext *context, XirEffectInput input, const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_program_input_root(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_refinement_capture_input(const XrXirCompileContext *context, XirEffectInput input, const struct XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_refinement_capture_current(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_context_refresh_input(const XrXirCompileContext *context, XirEffectInput input, const struct XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_context_refresh_current(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_context_operation_input(const XrXirCompileContext *context, const struct XrXirEffects *effects, XirEffectInput input, const XrXirTypes *types, XirEffectContextOperation operation, void *data) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_context_read_operation(context, effects, input, types, operation, data);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_reference_root_input(const XrXirCompileContext *context, const struct XrXirEffects *effects, XirEffectInput input, uint32_t function, uint32_t instruction, uint32_t *output) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    uint32_t candidate=0;
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_context_reference_current(context, effects, input.module, function, instruction, &candidate);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    if(status==XR_XIR_OK)*output=candidate;
    return status;
}

XR_FUNC XrXirStatus xir_effects_context_select_input(const XrXirCompileContext *context,
    const XrXirEffects *effects,const XirEffectContextInput *input,XirEffectContextView *output) {
    if(!input || !output)return XR_XIR_BAD_STRUCTURE;
    XirEffectContextView candidate={0};
    XirEffectInput current={input->source,input->construction,input->control};
    XrXirStatus status=effect_input_facts_match(context,current,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_context_read_select(context,effects,input,&candidate);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,current,effects->input_facts);
    if(status==XR_XIR_OK)*output=candidate;
    return status;
}
XR_FUNC XrXirStatus xir_effects_context_select_scoped_input(XirEffectContextScope *scope,
    const XirEffectContextInput *input,XirEffectContextView *output) {
    if(!scope)return XR_XIR_BAD_STRUCTURE;
    return effect_context_read_scoped(scope,input,output);
}

XR_FUNC XrXirStatus xir_effects_raw_source_terminal_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_source_raw_terminal(context,input.module,effects);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_receiving_terminal_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_source_receiving_terminal(context,input.module,effects);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_built_copy_match_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_source_copy_match(context,input.module,effects);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_built_copy_terminal_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_source_copy_terminal(context,input.module,effects);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_parameters_match_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_parameters_current(context,input.module,effects);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_local_errors_verify_input(const XrXirCompileContext *context,
    XirEffectInput input,const XrXirEffects *effects,XrXirDiagnostic *diagnostic) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK)status=effect_local_errors_current(context,input.module,effects,diagnostic);
    if(status==XR_XIR_OK)status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}
