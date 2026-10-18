/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_binding.c - Checked creator transfer into real lexical scopes
 */
#include "xanalyzer_visitor_internal.h"

static bool xa_visit_discard_binding(XaSymbol **input) {
    if (input && *input) {
        /* A rejected detached creator is ours; an existing scope binding is borrowed. */
        if (!(*input)->scope)
            xa_symbol_free(*input);
        *input = NULL;
    }
    return false;
}

XR_FUNC bool xa_visit_add_symbol_checked(XaInferContext *ctx, XaSymbol **input, int line) {
    XaSymbol *symbol = input ? *input : NULL;
    XrCompileState *state = xa_symbol_registry_state(ctx && ctx->analyzer ? ctx->analyzer->symbol_registry : NULL);
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return xa_visit_discard_binding(input);
    if (!ctx || !ctx->analyzer || !symbol) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return xa_visit_discard_binding(input);
    }
    XaScope *scope = ctx->analyzer->current_scope;
    if (!scope) {
        (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return xa_visit_discard_binding(input);
    }
    /* `target` is a compiler-owned namespace, not a shadowable prelude value.
     * Every source binding kind reaches this common definition boundary, so
     * locals, parameters, captures, patterns and declarations all fail closed
     * without teaching the lexer a target-specific rule. */
    if (!symbol->is_builtin && symbol->name &&
        strcmp(symbol->name, "target") == 0) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "Symbol 'target' redeclares the compiler-owned target namespace");
        XrLocation loc = {.file = ctx->file_path,
                          .line = line > 0 ? line : (int) symbol->location.line,
                          .column = (int) symbol->location.column};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR,
                                   XR_ERR_CMP_REDEFINED_VAR, msg, &loc);
        return xa_visit_discard_binding(input);
    }

    if (symbol->scope) {
        if (symbol->scope != scope) {
            (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            return xa_visit_discard_binding(input);
        }
        XaSymbol *bound = xa_symbol_registry_lookup(ctx->analyzer->symbol_registry, symbol->id);
        if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
            return xa_visit_discard_binding(input);
        if (bound != symbol) {
            (void) xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
            return xa_visit_discard_binding(input);
        }
        XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
        if (links && !links->file_path)
            links->file_path = ctx->file_path;
        return true;
    }

    XaSymbol *existing = symbol->name ? xa_scope_lookup_local(scope, symbol->name) : NULL;
    if (xr_compile_state_status(state) != XR_COMPILE_RESOURCE_OK)
        return xa_visit_discard_binding(input);
    if (existing) {
        bool same_source_symbol = existing->kind == symbol->kind &&
                                  existing->location.line == symbol->location.line &&
                                  existing->location.column == symbol->location.column;
        if (!same_source_symbol) {
            char msg[256];
            /* Result / Ordering are canonical builtin prelude enums bound to a
             * single type identity; they are reserved and cannot be
             * redeclared (the lowerer always resolves them to their builtin
             * slot, so allowing a redefinition would silently mis-resolve). */
            if (existing->is_builtin && existing->kind == XA_SYM_ENUM)
                snprintf(msg, sizeof(msg),
                         "'%s' is a builtin prelude enum and cannot be redeclared", symbol->name);
            else
                snprintf(msg, sizeof(msg), "Symbol '%s' is redefined in the same scope",
                         symbol->name);
            XrLocation loc = {.file = ctx->file_path,
                              .line = line > 0 ? line : (int) symbol->location.line,
                              .column = (int) symbol->location.column};
            xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_CMP_REDEFINED_VAR,
                                       msg, &loc);
            /* A duplicate function or method must stay out of the scope: two
             * bodies under one name make the error-set fixpoint oscillate
             * between them and never converge. Analysis proceeds on the first
             * definition alone; the diagnostic above already rejects the
             * program. */
            if (symbol->kind == XA_SYM_FUNCTION || symbol->kind == XA_SYM_METHOD)
                return xa_visit_discard_binding(input);
        }
    }
    if (xa_scope_add_symbol(scope, symbol) != XR_COMPILE_RESOURCE_OK)
        return xa_visit_discard_binding(input);
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, symbol);
    if (links && !links->file_path)
        links->file_path = ctx->file_path;
    return true;
}

