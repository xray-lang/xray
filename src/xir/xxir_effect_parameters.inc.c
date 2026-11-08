/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_parameters.inc.c - Declared callable parameter contracts and current-input validation
 *
 * KEY CONCEPT:
 *   Parameter lifetime kinds come from admitted declaration metadata.
 *   Receiving validates the stored contract against the shared numeric owner.
 */

XR_FUNC const XrXirFunctionEffectContract *xir_effects_contract(
    const XrXirEffects *effects, uint32_t function) {
    const XrXirFunctionEffectContract *contracts=effect_contracts_read(effects);
    return effects && contracts && function<effects->count?&contracts[function]:NULL;
}

static XrXirStatus effect_parameters_current(const XrXirCompileContext *work,
    const XrXirModule *module, const XrXirEffects *effects) {
    if (!xir_compile_context_valid(work) || !module || !effects ||
        effects->resources != work->resources || effects->count != module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!module->provenance || module->provenance->kind != XR_XIR_EVIDENCE_TEMPLATE)
        return XR_XIR_OK;
    if (!effect_contracts_read(effects) || !module->provenance->contracts ||
        module->provenance->contract_count != effects->count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t f = 0; f < effects->count; ++f) {

        const XrXirFunctionEffectContract *derived = &effect_contracts_read(effects)[f];
        const XrXirFunctionEffectContract *stored = &module->provenance->contracts[f];
        if (derived->parameter_count != stored->parameter_count) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t p = 0; p < derived->parameter_count; ++p) {

            if (derived->parameters[p].kind != stored->parameters[p].kind) return XR_XIR_BAD_TYPE;
        }

        if (derived->formula.constant_mask != stored->formula.constant_mask ||
            derived->formula.term_count != stored->formula.term_count) return XR_XIR_BAD_TYPE;
        for (uint32_t t = 0; t < derived->formula.term_count; ++t) {

            if (derived->formula.terms[t].kind != stored->formula.terms[t].kind ||
                derived->formula.terms[t].index != stored->formula.terms[t].index) return XR_XIR_BAD_TYPE;
        }
        for (uint32_t b = 0; b < stored->binding_count; ++b) {

            const XrXirEffectCallBinding *binding = &stored->bindings[b];
            if (binding->family != XR_XIR_EFFECT_BINDING_DIRECT &&
                binding->family != XR_XIR_EFFECT_BINDING_CAPTURE &&
                binding->family != XR_XIR_EFFECT_BINDING_REQUIREMENT) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xir_effect_call_binding_candidate(work,module,f,
                binding->instruction,binding->parameter);
            if (status != XR_XIR_OK) return status;
            const XrXirInstruction *call = &module->functions[f].instructions[binding->instruction];
            if (binding->family==XR_XIR_EFFECT_BINDING_REQUIREMENT) {
                if (call->op!=XR_XIR_CALL_REQUIREMENT) return XR_XIR_BAD_STRUCTURE;
                continue;
            }
            const XrXirFunctionEffectContract *callee = &effect_contracts_read(effects)[call->immediate];
            if (binding->parameter >= callee->parameter_count ||
                callee->parameters[binding->parameter].kind == XR_XIR_EFFECT_PARAMETER_FIXED)
                return XR_XIR_BAD_TYPE;
        }
    }
    return XR_XIR_OK;
}
