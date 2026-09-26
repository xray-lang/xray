/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_native_effect.c - Native (extern "C") declaration effect axioms
 */

#include "xa_native_effect.h"
#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../../module/xnative_package.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../core/xr_core_spec_gen.h"
#include "../../plan/semantic/xr_semantic_ids.h"
#include <string.h>

static bool native_text_equals(const char *text, const char *expected) {
    return text && expected && strcmp(text, expected) == 0;
}

bool xa_native_effect_is_bodyless_extern(const XaSymbol *symbol) {
    if (!symbol || !symbol->links.is_extern)
        return false;
    AstNode *decl = symbol->links.function_decl_node;
    if (!decl)
        return true;
    if (decl->type == AST_FUNCTION_DECL)
        return decl->as.function_decl.body == NULL;
    if (decl->type == AST_METHOD_DECL)
        return decl->as.method_decl.body == NULL;
    return true;
}

XaNativeEffectAxioms xa_native_effect_axioms(XaAnalyzer *analyzer, const XaSymbol *symbol) {
    XaNativeEffectAxioms axioms;
    memset(&axioms, 0, sizeof(axioms));
    if (!symbol || !symbol->name)
        return axioms;
    const XrNativePackagePlan *plan =
        xr_compiler_session_native_package_plan(analyzer ? analyzer->compiler_session : NULL);
    if (!plan || !plan->valid)
        return axioms;
    const XrNativeSymbol *native = xr_native_package_find_symbol(plan, symbol->name);
    if (!native || !native->contract.complete)
        return axioms;

    const XrNativeSymbolContract *contract = &native->contract;
    axioms.has_contract = true;
    if (native->provider.complete) {
        axioms.effects = XA_SEM_EFFECT_PROVIDER_CALL;
        return axioms;
    }
    /* Crossing the C boundary is itself an observable effect regardless of what
     * the callee does behind it. */
    axioms.effects = XA_SEM_EFFECT_FOREIGN;
    if (native_text_equals(contract->allocation, "may")) {
        axioms.allocates = true;
        axioms.effects |= XA_SEM_EFFECT_ALLOC;
    }
    /* A native symbol cannot be a generator, so its declared suspension is
     * always the scheduler-visible kind. */
    if (native_text_equals(contract->suspend, "may")) {
        axioms.suspends = true;
        axioms.effects |= XA_SEM_EFFECT_SCHED_SUSPEND;
    }
    if (native_text_equals(contract->blocking, "may"))
        axioms.effects |= XA_SEM_EFFECT_MAY_BLOCK;
    if (native_text_equals(contract->panic, "abort"))
        axioms.effects |= XA_SEM_EFFECT_ABORT;
    if (contract->io && !native_text_equals(contract->io, "none"))
        axioms.effects |= XA_SEM_EFFECT_IO;
    if (contract->sync && !native_text_equals(contract->sync, "none"))
        axioms.effects |= XA_SEM_EFFECT_SYNC;
    return axioms;
}

static bool provider_i64_type(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable &&
           type->scalar_rep == XR_NATIVE_I64;
}

XaProviderCallResolution xa_native_provider_call_resolve(XaAnalyzer *analyzer,
                                                         const XaSymbol *symbol,
                                                         const XrType *function_type,
                                                         XaProviderCallFact *out_fact) {
    if (out_fact)
        memset(out_fact, 0, sizeof(*out_fact));
    if (!analyzer || !symbol || !symbol->name)
        return XA_PROVIDER_CALL_NOT_BOUND;
    const XrNativePackagePlan *plan =
        xr_compiler_session_native_package_plan(analyzer->compiler_session);
    if (!plan || !plan->valid)
        return XA_PROVIDER_CALL_NOT_BOUND;
    const XrNativeSymbol *native = xr_native_package_find_symbol(plan, symbol->name);
    if (!native || !native->provider.complete)
        return XA_PROVIDER_CALL_NOT_BOUND;
    if (!xa_native_effect_is_bodyless_extern(symbol) || !native->contract.complete ||
        native->kind != XR_NATIVE_SYMBOL_FUNCTION || !function_type ||
        function_type->kind != XR_KIND_FUNCTION || function_type->function.is_variadic ||
        function_type->function.param_count != 1 || function_type->function.min_params != 1 ||
        xr_type_function_param_mode(function_type, 0) != XR_PARAM_READ ||
        !provider_i64_type(xr_type_function_param_type(function_type, 0)) ||
        !provider_i64_type(function_type->function.return_type))
        return XA_PROVIDER_CALL_INVALID;
    XaProviderCallFact fact = {
        .source_symbol_id = symbol->id,
        .effect_mask = XA_PROVIDER_CALL_EFFECT_MASK,
        .capability_mask = XA_PROVIDER_CALL_CAPABILITY_MASK,
        .call_abi = XA_PROVIDER_CALL_ABI_I64_TO_I64,
        .complete = 1,
    };
    XrFingerprint digest;
    if (!xr_stable_id_from_key(native->provider.contract_key, &fact.contract_id, &digest) ||
        !xr_stable_id_from_key(native->provider.operation_key, &fact.operation_id, &digest))
        return XA_PROVIDER_CALL_INVALID;
    if (out_fact)
        *out_fact = fact;
    return XA_PROVIDER_CALL_VERIFIED;
}
