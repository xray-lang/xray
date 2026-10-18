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
#include "xanalyzer_native_input.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../../module/xnative_package.h"
#include "../../toolchain/xcompiler_session.h"
#include <string.h>

static bool native_text_equals(XaAnalyzer *analyzer, const char *text, const char *expected) {
    if (!text || !expected)
        return false;
    if (xr_compile_state_work(xr_compile_session_compile_state(analyzer->compiler_session),
            strlen(expected) + 1) != XR_COMPILE_RESOURCE_OK)
        return false;
    return strcmp(text, expected) == 0;
}

XR_FUNC bool xa_native_effect_is_bodyless_extern(const XaSymbol *symbol) {
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

XR_FUNC XaNativeEffectAxioms xa_native_effect_axioms(XaAnalyzer *analyzer, const XaSymbol *symbol) {
    XaNativeEffectAxioms axioms;
    memset(&axioms, 0, sizeof(axioms));
    if (!symbol || !symbol->name)
        return axioms;
    const XrNativePackagePlan *plan = xa_analyzer_native_plan(analyzer);
    if (!plan || !plan->valid)
        return axioms;
    const XrNativeSymbol *native = NULL;
    if (xa_analyzer_find_native_symbol(analyzer, symbol->name, &native) != XR_MANIFEST_OK)
        return axioms;
    if (!native || !native->contract.complete)
        return axioms;

    const XrNativeSymbolContract *contract = &native->contract;
    axioms.has_contract = true;
    /* Crossing the C boundary is itself an observable effect regardless of what
     * the callee does behind it. */
    axioms.effects = XA_SEM_EFFECT_FOREIGN;
    if (native_text_equals(analyzer, contract->allocation, "may")) {
        axioms.allocates = true;
        axioms.effects |= XA_SEM_EFFECT_ALLOC;
    }
    /* A native symbol cannot be a generator, so its declared suspension is
     * always the scheduler-visible kind. */
    if (native_text_equals(analyzer, contract->suspend, "may")) {
        axioms.suspends = true;
        axioms.effects |= XA_SEM_EFFECT_SCHED_SUSPEND;
    }
    if (native_text_equals(analyzer, contract->blocking, "may"))
        axioms.effects |= XA_SEM_EFFECT_MAY_BLOCK;
    if (native_text_equals(analyzer, contract->panic, "abort"))
        axioms.effects |= XA_SEM_EFFECT_ABORT;
    if (contract->io && !native_text_equals(analyzer, contract->io, "none"))
        axioms.effects |= XA_SEM_EFFECT_IO;
    if (contract->sync && !native_text_equals(analyzer, contract->sync, "none"))
        axioms.effects |= XA_SEM_EFFECT_SYNC;
    if (xr_compile_session_resource_status(analyzer->compiler_session) != XR_COMPILE_RESOURCE_OK)
        return (XaNativeEffectAxioms){0};
    return axioms;
}
