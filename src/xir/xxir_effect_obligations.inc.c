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
#include "xxir_constraints.h"
#include "xxir_callable_root_obligations.inc.c"
static XrXirStatus declaration_effects_verify(const XrXirModule *module,
    XrXirCompileContext *remaining, XrXirDiagnostic *diagnostic, const XrXirEffects *prepared) {
    bool templated = module->provenance && module->provenance->kind == XR_XIR_EVIDENCE_TEMPLATE;
    if (!module->declarations) {
        if (!templated) return XR_XIR_OK;
        XrXirEffects *owned = NULL;
        XrXirStatus status = prepared ? XR_XIR_OK :
            xr_xir_compile_effects_infer_verified(remaining, module, &owned);
        if (status == XR_XIR_OK) status = xir_effects_parameters_match(remaining,
            module, prepared ? prepared : owned);
        xr_xir_compile_effects_free(owned);
        return status;
    }
    bool present = templated || module->declarations->implementations != NULL;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
        uint32_t owner = module->declarations->functions[f].cleanup_owner;
        if (module->declarations->functions[f].promises) present = true;
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {
            if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
            XrXirOp op = module->functions[f].instructions[i].op;
            if (op == XR_XIR_GO || op == XR_XIR_TASK_AWAIT || op == XR_XIR_FUNCTION_REF) present = true;
        }
        if (!owner) continue;
        present = true;
        *diagnostic = (XrXirDiagnostic){XR_XIR_OK, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        if (module->functions[f].result != XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        if (module->generics) {
            const XrXirGeneric *body = &module->generics[f], *parent = &module->generics[owner - 1];
            if (body->parameter_count != parent->parameter_count) return XR_XIR_BAD_TYPE;
            for (uint32_t p = 0; p < body->parameter_count; ++p) {
                if (!xir_compile_work(remaining, 1)) return XR_XIR_BUDGET;
                XrXirStatus status = xr_xir_compile_constraint_records_match(remaining, module->types, body->constraints[p], module->types, parent->constraints[p], body->parameter_count);
                if (status != XR_XIR_OK) return status;
            }
        }
    }
    if (!present) return XR_XIR_OK;
    XrXirEffects *effects = NULL;
    XrXirStatus status = prepared ? XR_XIR_OK : xr_xir_compile_effects_infer_verified(remaining, module, &effects);
    const XrXirEffects *facts = prepared ? prepared : effects;
    if (status == XR_XIR_OK) status = xir_effects_parameters_match(remaining, module, facts);
    if (status == XR_XIR_OK) status = declaration_callable_root_verify(module, facts, remaining, diagnostic);
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; status == XR_XIR_OK && i < function->instruction_count; ++i) {
            if (!xir_compile_work(remaining, 1)) { status = XR_XIR_BUDGET; break; }
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_GO) continue;
            status = xr_xir_effects_go_safe(facts, (uint32_t)op->immediate);
            XrXirDiagnosticReason reason = XR_XIR_DIAGNOSTIC_NONE;
            if (status == XR_XIR_BAD_TYPE) {
                const XrXirRootEffects *root = xr_xir_effects_root(facts, (uint32_t)op->immediate);
                if (!root) status = XR_XIR_BAD_STRUCTURE;
                else if (root->requires_root) reason = XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED;
                else if (root->unresolved) reason = XR_XIR_DIAGNOSTIC_GO_ROOT_UNRESOLVED;
            }
            if (status == XR_XIR_OK) status = xr_xir_effects_task_errors(facts, (uint32_t)op->immediate);
            if (status != XR_XIR_OK) *diagnostic = (XrXirDiagnostic){status, f, UINT32_MAX, i, reason};
        }
    }
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {
        if (!xir_compile_work(remaining, 1)) { status = XR_XIR_BUDGET; break; }
        bool cleanup = module->declarations->functions[f].cleanup_owner != 0;
        if (!cleanup && !module->declarations->functions[f].promises) continue;
        if (cleanup && xr_xir_effects_task_creation(facts, f) != XR_XIR_EFFECT_NONE) {
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
            status = XR_XIR_BAD_TYPE; break;
        }
        const XrXirFunctionEffects *fact = xr_xir_effects_function(facts, f);
        if ((cleanup && fact->throws == XR_XIR_EFFECT_MAY) || fact->suspend != XR_XIR_EFFECT_NONE) {
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
            diagnostic->reason = !cleanup ? XR_XIR_DIAGNOSTIC_NO_SUSPEND :
                fact->throws == XR_XIR_EFFECT_MAY ? XR_XIR_DIAGNOSTIC_CLEANUP_THROW : XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND;
            status = XR_XIR_BAD_TYPE;
            if (diagnostic->reason != XR_XIR_DIAGNOSTIC_CLEANUP_THROW) {
                const XrXirEffectWitness *witness = xr_xir_effects_suspend_witness(facts, f);
                if (!witness) { status = XR_XIR_BAD_STRUCTURE; break; }
                diagnostic->instruction = witness->instruction;
                const XrXirFunction *function = &module->functions[f];
                for (uint32_t b = 0; b < function->block_count; ++b) {
                    if (!xir_compile_work(remaining, 1)) { status = XR_XIR_BUDGET; break; }
                    XrXirBlock block = function->blocks[b];
                    if (witness->instruction >= block.first && witness->instruction - block.first < block.count) {
                        diagnostic->block = b; break;
                    }
                }
            }
        }
    }
    xr_xir_compile_effects_free(effects);
    return status;
}
