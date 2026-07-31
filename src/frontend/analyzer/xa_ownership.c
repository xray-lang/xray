/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_ownership.c - The ownership decision: alias state and live loans.
 *
 * Implements the model LANGUAGE_SPEC 2.13 states, over the evidence axes
 * declared in xa_ownership.h.  Two of the four axes live here:
 *
 *   Root aliasing -- an object-graph fact.  A second binding of the same root
 *   is recoverable (the alias dies at its last use); a reference written into
 *   a heap graph is not, because function-local analysis cannot observe that
 *   slot being overwritten.
 *
 *   Loans -- bounded place facts with a borrower and a last use.  One registry
 *   holds all three forms: a Slice view, a raw pointer borrow, and a closure
 *   that captured a root by reference.  They share a registry because they
 *   share the question they answer: is someone still holding this root?
 *
 * Liveness is decided by last use, not by lexical scope, for every form.
 *
 * The binding-state and capability axes stay with the visitors that compute
 * them: binding state is a CFG fact and capability is a declaration fact,
 * neither of which needs this registry.
 */

#include "xanalyzer_visitor_internal.h"
#include "xanalyzer_infer.h"
#include "xa_ownership.h"

XR_FUNC XaActiveLoan *xa_active_loan_for_borrower(XaInferContext *ctx, XaSymbol *view_sym) {
    if (!ctx || !view_sym)
        return NULL;
    for (XaActiveLoan *b = ctx->active_loans; b; b = b->next) {
        if (b->borrower_symbol == view_sym)
            return b;
    }
    return NULL;
}

static const char *xa_loan_kind_label(XaLoanKind kind) {
    switch (kind) {
        case XA_LOAN_RAW_READ:
        case XA_LOAN_RAW_WRITE:
            return "raw pointer borrow";
        case XA_LOAN_CAPTURE:
            return "closure capture";
        case XA_LOAN_READ:
        case XA_LOAN_WRITE:
            break;
    }
    return "Slice view";
}

static bool xa_owner_paths_may_overlap(const char *borrow_path, const char *mutation_path) {
    if (!borrow_path || !mutation_path)
        return true;
    return xa_path_is_same_or_nested(borrow_path, mutation_path) ||
           xa_path_is_same_or_nested(mutation_path, borrow_path);
}

static bool xa_active_loan_may_be_live_after_mutation(XaInferContext *ctx, XaActiveLoan *borrow) {
    if (!ctx || !ctx->analyzer || !borrow || !borrow->borrower_symbol ||
        !borrow->borrower_symbol->name)
        return true;
    if (ctx->loop_depth > 0 && borrow->loop_depth_at_creation < ctx->loop_depth)
        return true;
    const char *name = borrow->borrower_symbol->name;
    if (ctx->block_cursor_depth > 0) {
        int deepest = ctx->block_cursor_depth - 1;
        for (int depth = deepest; depth >= 0; depth--) {
            AstNode *block_node = ctx->block_cursor_nodes[depth];
            int stmt_index = ctx->block_cursor_indices[depth];
            AstNode **statements = NULL;
            int count = 0;
            if (!xa_block_node_statements(block_node, &statements, &count) || stmt_index < 0 ||
                stmt_index >= count)
                return true;
            if (depth == deepest && xa_node_uses_symbol_name(statements[stmt_index], name))
                return true;
            if (xa_block_uses_symbol_name_from(block_node, name, stmt_index + 1))
                return true;
        }
        return false;
    }

    AstNode *block_node = ctx->current_block_node;
    int stmt_index = ctx->current_block_stmt_index;
    AstNode **statements = NULL;
    int count = 0;
    if (!xa_block_node_statements(block_node, &statements, &count) || stmt_index < 0 ||
        stmt_index >= count)
        return true;
    if (xa_node_uses_symbol_name(statements[stmt_index], name))
        return true;
    return xa_block_uses_symbol_name_from(block_node, name, stmt_index + 1);
}

static XaSymbol *xa_find_live_strong_alias_in_scope(XaInferContext *ctx, XaScope *scope,
                                                    const XaSymbol *move_sym, XaRootId root,
                                                    bool *analysis_failed) {
    if (!scope || !root || (analysis_failed && *analysis_failed))
        return NULL;
    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);
    if (!symbols && xa_scope_count_symbols(scope) != 0) {
        if (analysis_failed)
            *analysis_failed = true;
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        XaSymbol *alias = symbols[i];
        if (!alias || alias == move_sym || alias->links.root_id != root ||
            alias->links.binding_use != XA_BINDING_LIVE)
            continue;
        if (xa_symbol_used_after_current_statement(ctx, alias)) {
            xr_free(symbols);
            return alias;
        }
    }
    xr_free(symbols);
    for (int i = 0; i < scope->child_count; i++) {
        XaSymbol *alias = xa_find_live_strong_alias_in_scope(ctx, scope->children[i], move_sym,
                                                             root, analysis_failed);
        if (alias || (analysis_failed && *analysis_failed))
            return alias;
    }
    return NULL;
}

XR_FUNC XaSymbol *xa_find_live_strong_alias_after_current(XaInferContext *ctx, XaSymbol *move_sym,
                                                          bool *analysis_failed) {
    if (analysis_failed)
        *analysis_failed = false;
    if (!ctx || !ctx->analyzer || !move_sym || move_sym->links.root_id == 0)
        return NULL;
    return xa_find_live_strong_alias_in_scope(ctx, ctx->analyzer->global_scope, move_sym,
                                              move_sym->links.root_id, analysis_failed);
}

static bool xa_mark_root_alias_state_in_scope(XaScope *scope, XaRootId root,
                                              XaRootAliasState state) {
    if (!scope || !root)
        return true;
    int count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols(scope, &count);
    if (!symbols && xa_scope_count_symbols(scope) != 0)
        return false;
    for (int i = 0; i < count; i++) {
        if (symbols[i] && symbols[i]->links.root_id == root)
            symbols[i]->links.root_alias = state;
    }
    xr_free(symbols);
    for (int i = 0; i < scope->child_count; i++) {
        if (!xa_mark_root_alias_state_in_scope(scope->children[i], root, state))
            return false;
    }
    return true;
}

XR_FUNC void xa_mark_root_alias_state(XaInferContext *ctx, XaRootId root, XaRootAliasState state) {
    if (!ctx || !ctx->analyzer || !root)
        return;
    if (!xa_mark_root_alias_state_in_scope(ctx->analyzer->global_scope, root, state)) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = ctx->current_block_node ? ctx->current_block_node->line : 0,
                          .column = 0};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "ownership alias evidence allocation failed "
                                   "(AnalysisResourceFailure)",
                                   &loc);
    }
}

XR_FUNC void xa_clear_active_loans_for_borrower(XaInferContext *ctx, XaSymbol *borrower_sym) {
    if (!ctx || !borrower_sym)
        return;
    XaActiveLoan **link = &ctx->active_loans;
    while (*link) {
        XaActiveLoan *cur = *link;
        if (cur->borrower_symbol == borrower_sym) {
            *link = cur->next;
            xr_free(cur->owner_path);
            xr_free(cur);
            continue;
        }
        link = &cur->next;
    }
}

XR_FUNC void xa_clear_active_loans_in_scope(XaInferContext *ctx, XaScope *scope) {
    if (!ctx || !scope)
        return;
    XaActiveLoan **link = &ctx->active_loans;
    while (*link) {
        XaActiveLoan *cur = *link;
        if (cur->borrower_scope == scope) {
            *link = cur->next;
            xr_free(cur->owner_path);
            xr_free(cur);
            continue;
        }
        link = &cur->next;
    }
}

/* Record one live loan of `owner` (or of the place `owner_path` inside it)
 * held by `borrower_sym`. The loan ends at the borrower's last use. */
static void xa_add_active_loan(XaInferContext *ctx, XaSymbol *borrower_sym, XaSymbol *owner,
                               const char *owner_path, XaLoanKind kind, AstNode *site) {
    if (!ctx || !borrower_sym || !owner || owner == borrower_sym)
        return;
    XaActiveLoan *loan = xr_calloc(1, sizeof(XaActiveLoan));
    if (!loan) {
        XrLocation loc = {.file = ctx->file_path,
                          .line = site ? site->line : 0,
                          .column = site ? site->column : 0};
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE,
                                   "loan evidence allocation failed (AnalysisResourceFailure)",
                                   &loc);
        return;
    }
    loan->owner_symbol = owner;
    if (owner_path && owner_path[0] != '\0')
        loan->owner_path = xr_strdup(owner_path);
    loan->borrower_symbol = borrower_sym;
    loan->borrower_scope = borrower_sym->scope;
    loan->loop_depth_at_creation = ctx->loop_depth;
    loan->loan.id = borrower_sym->id;
    loan->loan.root = owner->links.root_id;
    loan->loan.path.root = owner->links.root_id;
    loan->loan.path.precise = loan->owner_path != NULL;
    loan->loan.kind = kind;
    loan->loan.begin = site ? site->node_id + 1u : 0u;
    loan->loan.provenance = borrower_sym->id;
    loan->loan.borrower_symbol_id = borrower_sym->id;
    loan->loan.owner_symbol_id = owner->id;
    loan->loan.loop_depth_at_creation = (uint16_t) ctx->loop_depth;
    loan->next = ctx->active_loans;
    ctx->active_loans = loan;
}

XR_FUNC void xa_register_active_loan(XaInferContext *ctx, XaSymbol *borrower_sym, AstNode *value,
                                     XrType *value_type) {
    if (!ctx || !borrower_sym)
        return;
    xa_clear_active_loans_for_borrower(ctx, borrower_sym);
    bool is_pointer_borrow = false;
    bool is_span_borrow = xa_type_contains_span_view(value_type);
    if (!is_span_borrow && value_type && XR_TYPE_IS_POINTER(value_type)) {
        AstNode *source = value;
        while (source && (source->type == AST_GROUPING || source->type == AST_UNSAFE_EXPR ||
                          source->type == AST_AS_EXPR)) {
            if (source->type == AST_GROUPING)
                source = source->as.grouping;
            else if (source->type == AST_UNSAFE_EXPR)
                source = xa_unsafe_expr_result(source);
            else
                source = source->as.as_expr.expr;
        }
        is_pointer_borrow = xa_call_expr_preserves_owner_borrow(ctx, source, NULL) ||
                            xa_pointer_expr_has_owner_borrow(ctx, source);
    }
    if (!value || (!is_span_borrow && !is_pointer_borrow))
        return;
    char owner_path[512];
    XaSymbol *owner =
        xa_span_borrow_owner_path_for_expr(ctx, value, owner_path, sizeof(owner_path));
    if (!owner)
        return;
    XaLoanKind kind = is_pointer_borrow ? (value_type && value_type->ptr_is_mut ? XA_LOAN_RAW_WRITE
                                                                                : XA_LOAN_RAW_READ)
                                        : XA_LOAN_READ;
    xa_add_active_loan(ctx, borrower_sym, owner, owner_path, kind, value);
}

XR_FUNC void xa_record_pending_capture(XaInferContext *ctx, XaSymbol *captured) {
    if (!ctx || !captured)
        return;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, captured);
    if (!links || links->root_id == 0)
        return;
    for (int i = 0; i < ctx->pending_capture_count; i++) {
        if (ctx->pending_captures[i] == captured)
            return;
    }
    if (ctx->pending_capture_count >= XA_PENDING_CAPTURE_MAX) {
        /* The list cannot record this capture, so the root's alias state can
         * no longer be tracked. Fail closed instead of dropping it. */
        xa_mark_root_alias_state(ctx, links->root_id, XA_ROOT_ESCAPED);
        return;
    }
    ctx->pending_captures[ctx->pending_capture_count++] = captured;
}

XR_FUNC void xa_register_pending_capture_loans(XaInferContext *ctx, XaSymbol *borrower_sym,
                                               AstNode *site) {
    if (!ctx)
        return;
    for (int i = 0; i < ctx->pending_capture_count; i++) {
        XaSymbol *captured = ctx->pending_captures[i];
        if (borrower_sym && captured)
            xa_add_active_loan(ctx, borrower_sym, captured, NULL, XA_LOAN_CAPTURE, site);
    }
    ctx->pending_capture_count = 0;
}

XR_FUNC void xa_discard_pending_captures(XaInferContext *ctx) {
    if (ctx && ctx->closure_body_depth == 0)
        ctx->pending_capture_count = 0;
}

XR_FUNC void xa_escape_pending_captures(XaInferContext *ctx) {
    if (!ctx || !ctx->analyzer)
        return;
    for (int i = 0; i < ctx->pending_capture_count; i++) {
        XaSymbol *captured = ctx->pending_captures[i];
        XaSymbolLinks *links = captured ? xa_analyzer_get_links(ctx->analyzer, captured) : NULL;
        if (links && links->root_id != 0)
            xa_mark_root_alias_state(ctx, links->root_id, XA_ROOT_ESCAPED);
    }
    ctx->pending_capture_count = 0;
}

XR_FUNC void xa_check_active_loan_owner_path_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                      XaSymbol *owner_sym, const char *owner_path,
                                                      const char *operation) {
    if (!ctx || !ctx->analyzer || !owner_sym || !loc_node)
        return;
    for (XaActiveLoan *b = ctx->active_loans; b; b = b->next) {
        if (b->owner_symbol != owner_sym)
            continue;
        if (!xa_owner_paths_may_overlap(b->owner_path, owner_path))
            continue;
        if (!xa_active_loan_may_be_live_after_mutation(ctx, b)) {
            b->loan.last_use = loc_node->node_id + 1u;
            continue;
        }
        XrLocation loc = {
            .file = ctx->file_path, .line = loc_node->line, .column = loc_node->column};
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "cannot mutate owner '%s' while %s '%s' is active (OWN-E-LIVE-LOAN); end its "
                 "scope before %s",
                 owner_sym->name ? owner_sym->name : "?", xa_loan_kind_label(b->loan.kind),
                 b->borrower_symbol && b->borrower_symbol->name ? b->borrower_symbol->name : "?",
                 operation ? operation : "mutating the owner");
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_BORROW_CONFLICT,
                                   msg, &loc);
        return;
    }
}

XR_FUNC void xa_check_active_loan_owner_mutation(XaInferContext *ctx, AstNode *loc_node,
                                                 XaSymbol *owner_sym, const char *operation) {
    const char *owner_path = owner_sym && owner_sym->name ? owner_sym->name : NULL;
    xa_check_active_loan_owner_path_mutation(ctx, loc_node, owner_sym, owner_path, operation);
}

XR_FUNC void xa_note_owner_escapes_into_heap(XaInferContext *ctx, AstNode *value) {
    if (!ctx || !ctx->analyzer || !value)
        return;
    /* `move x` into a heap slot transfers the root instead of aliasing it, and
     * `copy(x)` stores an independent graph. Neither escapes the source. */
    AstNode *identity = xa_whole_binding_value(value);
    if (!identity || identity->type == AST_MOVE_EXPR)
        return;
    XaSymbol *source = xa_whole_binding_symbol(ctx, identity);
    if (!source)
        return;
    XaSymbolLinks *links = xa_analyzer_get_links(ctx->analyzer, source);
    if (!links || links->root_id == 0 || !xa_type_has_movable_root(links->type))
        return;
    xa_mark_root_alias_state(ctx, links->root_id, XA_ROOT_ESCAPED);
}
