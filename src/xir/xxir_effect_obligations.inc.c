/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_obligations.inc.c - Definition-context control obligations
 *
 * KEY CONCEPT:
 *   Known escaping errors and uncertain suspension are separate obligations.
 */
static XrXirStatus declaration_effects_verify(const XrXirModule *module,
    XrXirBudget *remaining, XrXirDiagnostic *diagnostic) {
    if (!module->declarations) return XR_XIR_OK;
    bool present = false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
        uint32_t owner = module->declarations->functions[f].cleanup_owner;
        if (module->declarations->functions[f].promises) present = true;
        if (!owner) continue;
        present = true;
        *diagnostic = (XrXirDiagnostic){XR_XIR_OK, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        if (module->functions[f].result != XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        if (module->generics) {
            const XrXirGeneric *body = &module->generics[f], *parent = &module->generics[owner - 1];
            if (body->parameter_count != parent->parameter_count) return XR_XIR_BAD_TYPE;
            for (uint32_t p = 0; p < body->parameter_count; ++p) {
                if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
                if (body->constraints[p] != parent->constraints[p]) return XR_XIR_BAD_TYPE;
            }
        }
    }
    if (!present) return XR_XIR_OK;
    XrXirEffects *effects = NULL;
    XrXirStatus status = xr_xir_effects_infer_verified(module, remaining, &effects);
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {
        if (!spend(&remaining->work, 1)) { status = XR_XIR_BUDGET; break; }
        bool cleanup = module->declarations->functions[f].cleanup_owner != 0;
        if (!cleanup && !module->declarations->functions[f].promises) continue;
        const XrXirFunctionEffects *fact = xr_xir_effects_function(effects, f);
        if ((cleanup && fact->throws == XR_XIR_EFFECT_MAY) || fact->suspend != XR_XIR_EFFECT_NONE) {
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
            diagnostic->reason = !cleanup ? XR_XIR_DIAGNOSTIC_NO_SUSPEND :
                fact->throws == XR_XIR_EFFECT_MAY ? XR_XIR_DIAGNOSTIC_CLEANUP_THROW : XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND;
            status = XR_XIR_BAD_TYPE;
            if (diagnostic->reason != XR_XIR_DIAGNOSTIC_CLEANUP_THROW) {
                const XrXirEffectWitness *witness = xr_xir_effects_suspend_witness(effects, f);
                if (!witness) { status = XR_XIR_BAD_STRUCTURE; break; }
                diagnostic->instruction = witness->instruction;
                const XrXirFunction *function = &module->functions[f];
                for (uint32_t b = 0; b < function->block_count; ++b) {
                    if (!spend(&remaining->work, 1)) { status = XR_XIR_BUDGET; break; }
                    XrXirBlock block = function->blocks[b];
                    if (witness->instruction >= block.first && witness->instruction - block.first < block.count) {
                        diagnostic->block = b; break;
                    }
                }
            }
        }
    }
    xr_xir_effects_free(effects);
    return status;
}
