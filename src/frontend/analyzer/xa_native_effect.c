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
