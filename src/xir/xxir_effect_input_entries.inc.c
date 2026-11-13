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

XR_FUNC XrXirStatus xir_effects_source_terminal_input(const XrXirCompileContext *context, XirEffectInput input, const XrXirEffects *effects) {
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_source_input_terminal(context, input.module, effects);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    return status;
}

XR_FUNC XrXirStatus xir_effects_reference_root_input(const XrXirCompileContext *context, const struct XrXirEffects *effects, XirEffectInput input, uint32_t function, uint32_t instruction, uint32_t *output) {
    if(!output)return XR_XIR_BAD_STRUCTURE;
    uint32_t candidate=0;
    XrXirStatus status=effect_input_facts_match(context,input,effects?effects->input_facts:NULL);
    if(status==XR_XIR_OK) status=effect_checked_reference_current(context, effects, input.module, function, instruction, &candidate);
    if(status==XR_XIR_OK) status=effect_input_facts_match(context,input,effects->input_facts);
    if(status==XR_XIR_OK)*output=candidate;
    return status;
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

XR_FUNC XrXirStatus xir_effects_module_borrows_verify_input(const XrXirCompileContext *context,
    XirEffectInput input, const XrXirEffects *effects, XrXirDiagnostic *diagnostic) {
    XrXirStatus status = effect_input_facts_match(context, input, effects ? effects->input_facts : NULL);
    if (status == XR_XIR_OK) status = effect_module_borrows_verify(context, input.module, effects, diagnostic);
    if (status == XR_XIR_OK) status = effect_input_facts_match(context, input, effects->input_facts);
    return status;
}
