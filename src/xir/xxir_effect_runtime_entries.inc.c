/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_runtime_entries.inc.c - Published Cell roles and root entry views
 *
 * KEY CONCEPT:
 *   The canonical body, Cell proof and entry outcomes belong to one Final.
 *   A Program does not construct or retain another role inventory.
 */
static const EffectInvocationFinal *effect_runtime_entry_final(const XrXirEffects *effects,
    uint32_t function) {
    const EffectFinalOutcomes *outcomes=effect_final_outcomes(effects);
    if (!outcomes || function>=outcomes->count) return NULL;
    const EffectInvocationCertificate *certificate=effects->invocations;
    const EffectInvocationFinal *final=certificate->final;
    if (!effect_body_material_shape(certificate->body) ||
        certificate->body->resources!=effects->resources ||
        effect_body_count(certificate->body)!=outcomes->count ||
        !certificate->body->functions || !final->cells ||
        final->material!=certificate->body || final->cells!=certificate->body->cells)
        return NULL;
    return final;
}
XR_FUNC XrXirStatus xir_effects_runtime_cell_role(const XrXirEffects *effects,
    uint32_t function,uint32_t parameter,XrXirCellProofRole *output) {
    if (!output || *output!=XR_XIR_CELL_PROOF_UNKNOWN) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *final=effect_runtime_entry_final(effects,function);
    if (!final || parameter>=final->material->functions[function].parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirCellProofRole role=xr_xir_cell_provenance_role(final->cells,function,parameter);
    if ((uint32_t)role>XR_XIR_CELL_PROOF_LEXICAL_CLEANUP)
        return XR_XIR_BAD_STRUCTURE;
    *output=role;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_effects_runtime_entry_root(const XrXirEffects *effects,
    uint32_t function,XirEffectEntryRootView *output) {
    if (!output || output->intrinsic_mask || output->parameters || output->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *final=effect_runtime_entry_final(effects,function);
    if (!final) return XR_XIR_BAD_STRUCTURE;
    const XirEffectEntryRootView *entry=&final->outcomes->entry_roots[function];
    if (entry->parameter_count>final->material->functions[function].parameter_count ||
        (entry->parameter_count && !entry->parameters) ||
        (entry->intrinsic_mask & ~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)))
        return XR_XIR_BAD_STRUCTURE;
    *output=*entry;return XR_XIR_OK;
}
