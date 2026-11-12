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
/* The Source entry follows its synchronous access callback. Raw and external
 * collectors complete the same access admission using their fresh owner. */
typedef enum EffectDeclarationAdmission {
    EFFECT_DECLARATION_OWNED, EFFECT_DECLARATION_OPTIONAL, EFFECT_DECLARATION_SOURCE
} EffectDeclarationAdmission;

static bool declaration_view_access_needed(const XrXirModule *module) {
    for (uint32_t t=0;module->types && t<module->types->count;++t) {
        XrXirTypeKind kind=module->types->nodes[t].kind;
        if (kind==XR_XIR_TYPE_SLICE || kind==XR_XIR_TYPE_MUT_SLICE ||
            kind==XR_XIR_TYPE_PTR || kind==XR_XIR_TYPE_MUT_PTR) return true;
    }
    return false;
}

static XrXirStatus declaration_effects_verify(XirEffectInput input,
    XrXirCompileContext *remaining, XrXirDiagnostic *diagnostic, const XrXirEffects *prepared, XrXirEffects **output, EffectDeclarationAdmission admission) {
    const XrXirModule *module=input.module;
    bool require_owned=admission!=EFFECT_DECLARATION_OPTIONAL;
    bool access=admission!=EFFECT_DECLARATION_SOURCE && declaration_view_access_needed(module);
    XrXirStatus local=prepared?xir_effects_local_errors_verify_input(remaining,input,prepared,diagnostic):
        xir_compile_local_errors_verify(remaining,module,diagnostic);
    if (local!=XR_XIR_OK) return local;
    bool templated = module->provenance && module->provenance->kind == XR_XIR_EVIDENCE_TEMPLATE;
    if (output && (*output || prepared)) return XR_XIR_BAD_STRUCTURE;
    if (!module->declarations) {
        if (!access && !templated && (!output || !require_owned)) return XR_XIR_OK;
        XrXirEffects *owned = NULL;
        XrXirStatus status = prepared ? XR_XIR_OK :
            xir_effects_infer_lifetimes_input(remaining,input,&owned,diagnostic);
        if (status == XR_XIR_OK && templated) status = xir_effects_parameters_match_input(remaining,
            input, prepared ? prepared : owned);
        if (status==XR_XIR_OK && access)
            status=xir_compile_view_cell_access_verified_input(remaining,input,prepared?prepared:owned,diagnostic);
        if (status==XR_XIR_OK && output) { *output=owned;owned=NULL; }
        xr_xir_compile_effects_free(owned);
        return status;
    }
    bool present = access || templated || module->declarations->implementations != NULL;
    for (uint32_t f = 0; f < module->function_count; ++f) {

        uint32_t owner = module->declarations->functions[f].cleanup_owner;
        if (module->declarations->functions[f].promises & XR_XIR_FUNCTION_NO_SUSPEND) present = true;
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i) {

            XrXirOp op = module->functions[f].instructions[i].op;
            if (xr_xir_op_is_spawn(op) || op == XR_XIR_TASK_AWAIT || op==XR_XIR_TASK_AWAIT_RESULT || op == XR_XIR_FUNCTION_REF || op == XR_XIR_CLASS_METHOD_REF ||
                op==XR_XIR_GENERATOR_NEW || op==XR_XIR_ITERATOR_HAS_NEXT || (op==XR_XIR_ITERATOR_NEXT || op==XR_XIR_ITERATOR_NTH || op==XR_XIR_ITERATOR_CLOSE)) present = true;
        }
        if (!owner) continue;
        present = true;
        *diagnostic = (XrXirDiagnostic){XR_XIR_OK, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        if (module->functions[f].result != XR_XIR_UNIT) return XR_XIR_BAD_TYPE;
        if (module->generics) {
            const XrXirGeneric *body = &module->generics[f], *parent = &module->generics[owner - 1];
            if (body->parameter_count != parent->parameter_count) return XR_XIR_BAD_TYPE;
            for (uint32_t p = 0; p < body->parameter_count; ++p) {

                XrXirStatus status = xr_xir_compile_constraint_records_match(remaining, module->types, body->constraints[p], module->types, parent->constraints[p], body->parameter_count);
                if (status != XR_XIR_OK) return status;
            }
        }
    }
    if (!present) return output && require_owned ? xir_effects_infer_lifetimes_input(remaining,input,output,diagnostic) : XR_XIR_OK;
    XrXirEffects *effects = NULL;
    XrXirStatus status = prepared ? XR_XIR_OK : xir_effects_infer_lifetimes_input(remaining,input,&effects,diagnostic);
    const XrXirEffects *facts = prepared ? prepared : effects;
    if (status == XR_XIR_OK) status = xir_effects_parameters_match_input(remaining, input, facts);
    if (status == XR_XIR_OK && access)
        status=xir_compile_view_cell_access_verified_input(remaining,input,facts,diagnostic);
    if (status == XR_XIR_OK) status = declaration_callable_root_verify(input, facts, remaining, diagnostic);
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; status == XR_XIR_OK && i < function->instruction_count; ++i) {

            const XrXirInstruction *op = &function->instructions[i];
            if (!xr_xir_op_is_spawn(op->op)) continue;
            XrXirRootEffects root = {0};
            status = xir_effects_spawn_read(remaining, facts, f, i, &root);
            XrXirDiagnosticReason reason = XR_XIR_DIAGNOSTIC_NONE;
            if (status == XR_XIR_BAD_TYPE) {
                if (root.requires_root) reason = XR_XIR_DIAGNOSTIC_GO_ROOT_REQUIRED;
                else if (root.unresolved) reason = XR_XIR_DIAGNOSTIC_GO_ROOT_UNRESOLVED;
            }
            if (status == XR_XIR_OK) status = xir_compile_worker_error_verify(remaining,module,(uint32_t)op->immediate);
            if (status != XR_XIR_OK) *diagnostic = (XrXirDiagnostic){status, f, UINT32_MAX, i, reason};
        }
    }
    for (uint32_t f = 0; status == XR_XIR_OK && f < module->function_count; ++f) {

        bool cleanup = module->declarations->functions[f].cleanup_owner != 0;
        if (!cleanup && !(module->declarations->functions[f].promises & XR_XIR_FUNCTION_NO_SUSPEND)) continue;
        XrXirEffect task=XR_XIR_EFFECT_UNKNOWN;
        XrXirEffectFacts fact_read=xir_effects_declaration_read(remaining,facts,f,&task);
        if (fact_read.status!=XR_XIR_OK) return fact_read.status;
        if (cleanup && task != XR_XIR_EFFECT_NONE) {
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, UINT32_MAX,
                XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND};
            status = XR_XIR_BAD_TYPE; break;
        }
        const XrXirFunctionEffects *fact=(fact_read.status==XR_XIR_OK?&fact_read.control:NULL);
        if (!fact) { status=fact_read.status;break; }
        if ((cleanup && fact->throws == XR_XIR_EFFECT_MAY) || fact->suspend != XR_XIR_EFFECT_NONE) {
            *diagnostic = (XrXirDiagnostic){XR_XIR_BAD_TYPE, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
            diagnostic->reason = !cleanup ? XR_XIR_DIAGNOSTIC_NO_SUSPEND :
                fact->throws == XR_XIR_EFFECT_MAY ? XR_XIR_DIAGNOSTIC_CLEANUP_THROW : XR_XIR_DIAGNOSTIC_CLEANUP_SUSPEND;
            status = XR_XIR_BAD_TYPE;
            if (diagnostic->reason != XR_XIR_DIAGNOSTIC_CLEANUP_THROW) {
                XrXirEffectFacts witness_read=fact_read;
                const XrXirEffectWitness *witness=(witness_read.status==XR_XIR_OK && witness_read.suspend_witness.cause!=0?&witness_read.suspend_witness:NULL);
                if (!witness) { status = XR_XIR_BAD_STRUCTURE; break; }
                diagnostic->instruction = witness->instruction;
                const XrXirFunction *function = &module->functions[f];
                for (uint32_t b = 0; b < function->block_count; ++b) {

                    XrXirBlock block = function->blocks[b];
                    if (witness->instruction >= block.first && witness->instruction - block.first < block.count) {
                        diagnostic->block = b; break;
                    }
                }
            }
        }
    }
    if (status==XR_XIR_OK && output) { *output=effects;effects=NULL; }
    xr_xir_compile_effects_free(effects);
    return status;
}
