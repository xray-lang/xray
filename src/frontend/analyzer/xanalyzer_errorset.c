/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_errorset.c - Error set inference pass (Pass 3)
 *
 * After Pass 2 (type inference) has resolved all types, this pass
 * walks the AST to infer which enum error types each function may
 * throw.  The result is interned in XaAnalyzer.effect_db and stored
 * as XaSymbolLinks.effect_id.
 *
 * Algorithm:
 *   1. Collect all function symbols in topological order.
 *   2. For each function, walk its body:
 *      - `throw E.Case` => add E to error set
 *      - call to fallible function => union callee's error set
 *      - `catch (e) {}` => subtract caught errors
 *   3. Fixpoint iteration for recursive/mutually-recursive functions.
 */

#include "xanalyzer_errorset.h"
#include "xanalyzer_visitor.h"
#include "xa_effect_db.h"
#include "xa_selection.h"
#include "xtype_ref_resolve.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xmalloc.h"
#include <string.h>

static void es_summary_add_enum_all(XaEffectDatabase *db, XaEffectSummary *summary,
                                    XrType *enum_type) {
    if (!db || !summary || !enum_type)
        return;
    XaErrorTypeId type_id = xa_effect_db_register_error_enum(db, enum_type);
    if (type_id == XA_ERROR_TYPE_NONE)
        return;
    xa_effect_summary_add_all_variants(db, summary, type_id);
}

static void es_summary_add_enum_case(XaEffectDatabase *db, XaEffectSummary *summary,
                                     XrType *enum_type, uint32_t case_index) {
    if (!db || !summary || !enum_type)
        return;
    XaErrorTypeId type_id = xa_effect_db_register_error_enum(db, enum_type);
    if (type_id == XA_ERROR_TYPE_NONE)
        return;
    XaErrorVariantId variant_id = case_index;
    const XrEnumLayout *layout = enum_type->enum_type.layout;
    if (layout && case_index >= layout->variant_count)
        return;
    xa_effect_summary_add_variant(db, summary, type_id, variant_id);
}

/* ========== Internal Context ========== */

typedef struct FunctionValueTarget {
    XaSymbol *symbol;
    AstNode *function_expr;
    XaSymbol *target_symbols[8];
    AstNode *target_function_exprs[8];
    int target_count;
} FunctionValueTarget;

typedef struct FunctionValueAliasState {
    uint32_t ids[128];
    FunctionValueTarget targets[128];
    int count;
} FunctionValueAliasState;

typedef struct CatchAliasState {
    bool binding_is_caught;
    uint32_t alias_ids[64];
    const char *alias_names[64];
    int alias_count;
} CatchAliasState;

typedef struct FunctionReturnTargetEntry {
    uint32_t function_id;
    FunctionValueTarget target;
    bool seen;
    bool unknown;
} FunctionReturnTargetEntry;

typedef struct FunctionExprCaptureEntry {
    AstNode *function_expr;
    FunctionValueAliasState state;
} FunctionExprCaptureEntry;

typedef struct ErrorSetCtx {
    XaAnalyzer *analyzer;
    XaEffectSummary *current_summary; /* Effect summary being built for current function */
    const char *current_catch_var;    /* Catch variable currently in scope, if any */
    uint32_t current_catch_symbol_id; /* Symbol id for current_catch_var */
    bool current_catch_binding_is_caught;
    uint32_t current_catch_alias_ids[64];
    const char *current_catch_alias_names[64];
    int current_catch_alias_count;
    int current_catch_alias_control_depth;
    uint32_t function_value_alias_ids[128];
    FunctionValueTarget function_value_alias_targets[128];
    int function_value_alias_count;
    int function_value_control_depth;
    uint32_t function_value_mutation_ids[128];
    int function_value_mutation_count;
    int function_value_mutation_depth;
    FunctionReturnTargetEntry *function_return_targets;
    int function_return_target_count;
    int function_return_target_capacity;
    FunctionExprCaptureEntry *function_expr_captures;
    int function_expr_capture_count;
    int function_expr_capture_capacity;
    int callsite_inline_depth;
    FunctionValueTarget current_return_target;
    bool current_return_target_seen;
    bool current_return_target_unknown;
    XaEffectSummary *current_caught; /* Effect subset caught by current catch clause */
    XaSymbol *current_func;          /* Current function symbol */
    bool changed;                    /* Fixpoint: did anything change this iteration? */
} ErrorSetCtx;

static bool es_summary_add_enum_selection(ErrorSetCtx *ctx, const XaSelection *sel) {
    if (!ctx || !sel || sel->kind != XA_SEL_ENUM_MEMBER)
        return false;
    XrType *enum_type = sel->result_type;
    if ((!enum_type || !XR_TYPE_IS_ENUM(enum_type)) && sel->target_symbol)
        enum_type = sel->target_symbol->links.type;
    if (!enum_type || !XR_TYPE_IS_ENUM(enum_type))
        return false;
    if (sel->field_index >= 0)
        es_summary_add_enum_case(ctx->analyzer->effect_db, ctx->current_summary, enum_type,
                                 (uint32_t) sel->field_index);
    else
        es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary, enum_type);
    return true;
}

/* ========== Forward Declarations ========== */

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_block(ErrorSetCtx *ctx, AstNode *node);
static FunctionValueTarget resolve_call_target_depth(ErrorSetCtx *ctx, AstNode *callee, int depth);

/* ========== Helpers ========== */

static XaSymbol *resolve_func_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_METHOD_DECL) {
        XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
        if (scope && scope->function_symbol &&
            (scope->function_symbol->kind == XA_SYM_FUNCTION ||
             scope->function_symbol->kind == XA_SYM_METHOD))
            return scope->function_symbol;
        return NULL;
    }
    if (node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *fn = &node->as.function_decl;
        XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
        if (scope && scope->function_symbol &&
            (scope->function_symbol->kind == XA_SYM_FUNCTION ||
             scope->function_symbol->kind == XA_SYM_METHOD))
            return scope->function_symbol;
        if (fn->symbol_id != 0) {
            XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, fn->symbol_id);
            if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD))
                return sym;
        }
        const char *name = fn->name;
        if (name) {
            XaSymbol *sym = xa_scope_lookup(analyzer->global_scope, name);
            if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD))
                return sym;
        }
    }
    return NULL;
}

static AstNode *function_like_body(AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL)
        return node->as.function_decl.body;
    if (node->type == AST_METHOD_DECL)
        return node->as.method_decl.body;
    return NULL;
}

static AstNode *identity_source(AstNode *expr) {
    while (expr) {
        switch (expr->type) {
            case AST_GROUPING:
                expr = expr->as.grouping;
                break;
            case AST_FORCE_UNWRAP:
                expr = expr->as.unary.operand;
                break;
            case AST_AS_EXPR:
                expr = expr->as.as_expr.expr;
                break;
            default:
                return expr;
        }
    }
    return NULL;
}

static XaSymbol *lookup_variable_symbol(XaAnalyzer *analyzer, AstNode *node) {
    node = identity_source(node);
    if (!analyzer || !node || node->type != AST_VARIABLE || !node->as.variable.name)
        return NULL;

    if (node->as.variable.symbol_id != 0) {
        XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, node->as.variable.symbol_id);
        if (sym)
            return sym;
    }

    XaSymbol *sym = xa_analyzer_lookup(analyzer, node->as.variable.name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(analyzer, node->as.variable.name, analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(analyzer, node->as.variable.name);
    return sym;
}

static bool symbol_has_function_type(XaSymbol *sym) {
    if (!sym)
        return false;
    if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)
        return true;
    XrType *type = sym->links.type;
    return type && XR_TYPE_IS_FUNCTION(type);
}

static FunctionValueTarget function_value_target_none(void) {
    FunctionValueTarget target;
    target.symbol = NULL;
    target.function_expr = NULL;
    memset(target.target_symbols, 0, sizeof(target.target_symbols));
    memset(target.target_function_exprs, 0, sizeof(target.target_function_exprs));
    target.target_count = 0;
    return target;
}

static FunctionValueTarget function_value_target_symbol(XaSymbol *sym) {
    FunctionValueTarget target = function_value_target_none();
    target.symbol = sym;
    if (sym && (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)) {
        target.target_symbols[0] = sym;
        target.target_count = 1;
    }
    return target;
}

static FunctionValueTarget function_value_target_expr(AstNode *function_expr) {
    FunctionValueTarget target = function_value_target_none();
    target.function_expr = function_expr;
    if (function_expr) {
        target.target_function_exprs[0] = function_expr;
        target.target_count = 1;
    }
    return target;
}

static bool function_value_target_is_exact(FunctionValueTarget target) {
    return target.target_count > 0;
}

static bool function_value_target_equal(FunctionValueTarget a, FunctionValueTarget b) {
    if (a.target_count != b.target_count)
        return false;
    for (int i = 0; i < a.target_count; i++) {
        bool found = false;
        for (int j = 0; j < b.target_count; j++) {
            if (a.target_symbols[i] == b.target_symbols[j] &&
                a.target_function_exprs[i] == b.target_function_exprs[j]) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static bool function_value_target_add(FunctionValueTarget *target, XaSymbol *sym,
                                      AstNode *function_expr) {
    if (!target || (!sym && !function_expr))
        return false;
    for (int i = 0; i < target->target_count; i++) {
        if (target->target_symbols[i] == sym && target->target_function_exprs[i] == function_expr)
            return true;
    }
    if (target->target_count >= 8)
        return false;
    int slot = target->target_count++;
    target->target_symbols[slot] = sym;
    target->target_function_exprs[slot] = function_expr;
    if (slot == 0) {
        target->symbol = sym;
        target->function_expr = function_expr;
    }
    return true;
}

static FunctionValueTarget function_value_target_merge(FunctionValueTarget a,
                                                       FunctionValueTarget b) {
    FunctionValueTarget merged = function_value_target_none();
    if (!function_value_target_is_exact(a) || !function_value_target_is_exact(b))
        return merged;
    for (int i = 0; i < a.target_count; i++) {
        if (!function_value_target_add(&merged, a.target_symbols[i], a.target_function_exprs[i]))
            return function_value_target_none();
    }
    for (int i = 0; i < b.target_count; i++) {
        if (!function_value_target_add(&merged, b.target_symbols[i], b.target_function_exprs[i]))
            return function_value_target_none();
    }
    return merged;
}

static bool expr_has_function_type(ErrorSetCtx *ctx, AstNode *expr) {
    if (!ctx || !ctx->analyzer || !expr)
        return false;
    XrType *type = xa_analyzer_get_node_type(ctx->analyzer, expr);
    return type && XR_TYPE_IS_FUNCTION(type);
}

static bool current_function_returns_function(ErrorSetCtx *ctx) {
    if (!ctx || !ctx->current_func)
        return false;
    XaSymbolLinks *links = &ctx->current_func->links;
    XrType *return_type = links->return_type;
    if (!return_type && links->type && XR_TYPE_IS_FUNCTION(links->type))
        return_type = links->type->function.return_type;
    return return_type && XR_TYPE_IS_FUNCTION(return_type);
}

static void record_current_function_return_target(ErrorSetCtx *ctx, FunctionValueTarget target,
                                                  bool unknown) {
    if (!ctx || !ctx->current_func)
        return;
    ctx->current_return_target_seen = true;
    if (unknown || !function_value_target_is_exact(target)) {
        ctx->current_return_target = function_value_target_none();
        ctx->current_return_target_unknown = true;
        return;
    }
    if (!function_value_target_is_exact(ctx->current_return_target)) {
        ctx->current_return_target = target;
        return;
    }
    FunctionValueTarget merged = function_value_target_merge(ctx->current_return_target, target);
    if (!function_value_target_is_exact(merged)) {
        ctx->current_return_target = function_value_target_none();
        ctx->current_return_target_unknown = true;
        return;
    }
    ctx->current_return_target = merged;
}

static FunctionReturnTargetEntry *lookup_function_return_target_entry(ErrorSetCtx *ctx,
                                                                      XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return NULL;
    for (int i = 0; i < ctx->function_return_target_count; i++) {
        if (ctx->function_return_targets[i].function_id == sym->id)
            return &ctx->function_return_targets[i];
    }
    return NULL;
}

static FunctionValueTarget lookup_function_return_target(ErrorSetCtx *ctx, XaSymbol *sym,
                                                         bool *seen, bool *unknown) {
    if (seen)
        *seen = false;
    if (unknown)
        *unknown = false;
    FunctionReturnTargetEntry *entry = lookup_function_return_target_entry(ctx, sym);
    if (!entry)
        return function_value_target_none();
    if (seen)
        *seen = entry->seen;
    if (unknown)
        *unknown = entry->unknown;
    return entry->target;
}

static void store_function_return_target(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return;
    bool seen = ctx->current_return_target_seen;
    bool unknown = ctx->current_return_target_unknown;
    FunctionValueTarget target =
        unknown ? function_value_target_none() : ctx->current_return_target;
    if (!seen && !unknown)
        return;

    FunctionReturnTargetEntry *entry = lookup_function_return_target_entry(ctx, sym);
    if (!entry) {
        if (ctx->function_return_target_count >= ctx->function_return_target_capacity) {
            int new_cap = ctx->function_return_target_capacity == 0
                              ? 32
                              : ctx->function_return_target_capacity * 2;
            XR_REALLOC_OR_ABORT(ctx->function_return_targets,
                                (size_t) new_cap * sizeof(FunctionReturnTargetEntry),
                                "function return target grow");
            ctx->function_return_target_capacity = new_cap;
        }
        entry = &ctx->function_return_targets[ctx->function_return_target_count++];
        memset(entry, 0, sizeof(*entry));
        entry->function_id = sym->id;
        ctx->changed = true;
    } else if (entry->seen != seen || entry->unknown != unknown ||
               !function_value_target_equal(entry->target, target)) {
        ctx->changed = true;
    }

    entry->seen = seen;
    entry->unknown = unknown;
    entry->target = target;
}

static XaSymbol *lookup_symbol_by_id(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || !ctx->analyzer || symbol_id == 0)
        return NULL;
    return xa_scope_lookup_by_id(ctx->analyzer->global_scope, symbol_id);
}

static XaSymbol *lookup_assignment_symbol(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !ctx->analyzer || !assign)
        return NULL;
    XaSymbol *sym = lookup_symbol_by_id(ctx, assign->symbol_id);
    if (sym || !assign->name)
        return sym;
    sym = xa_analyzer_lookup(ctx->analyzer, assign->name);
    if (!sym)
        sym = xa_analyzer_lookup_in_scope(ctx->analyzer, assign->name, ctx->analyzer->global_scope);
    if (!sym)
        sym = xa_analyzer_lookup_deep(ctx->analyzer, assign->name);
    return sym;
}

static XaSymbol *resolve_function_alias_target(XaAnalyzer *analyzer, XaSymbol *sym, int depth) {
    if (!analyzer || !sym)
        return NULL;
    if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD)
        return sym;
    if (depth >= 32 || !sym->is_const || sym->is_rebindable || !sym->links.const_initializer)
        return sym;
    if (!symbol_has_function_type(sym))
        return sym;

    AstNode *source = identity_source(sym->links.const_initializer);
    if (!source || source->type != AST_VARIABLE)
        return sym;

    XaSymbol *source_sym = lookup_variable_symbol(analyzer, source);
    if (!source_sym || source_sym == sym)
        return sym;
    return resolve_function_alias_target(analyzer, source_sym, depth + 1);
}

static FunctionValueTarget lookup_function_value_alias_target(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return function_value_target_none();
    for (int i = ctx->function_value_alias_count - 1; i >= 0; i--) {
        if (ctx->function_value_alias_ids[i] == sym->id)
            return ctx->function_value_alias_targets[i];
    }
    return function_value_target_none();
}

static void set_function_value_alias_target(ErrorSetCtx *ctx, XaSymbol *sym,
                                            FunctionValueTarget target) {
    if (!ctx || !sym || sym->id == 0)
        return;
    for (int i = 0; i < ctx->function_value_alias_count; i++) {
        if (ctx->function_value_alias_ids[i] == sym->id) {
            ctx->function_value_alias_targets[i] = target;
            return;
        }
    }
    if (ctx->function_value_alias_count >= 128)
        return;
    int slot = ctx->function_value_alias_count++;
    ctx->function_value_alias_ids[slot] = sym->id;
    ctx->function_value_alias_targets[slot] = target;
}

static void invalidate_function_value_alias_target(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || symbol_id == 0)
        return;
    for (int i = 0; i < ctx->function_value_alias_count; i++) {
        if (ctx->function_value_alias_ids[i] == symbol_id)
            ctx->function_value_alias_targets[i] = function_value_target_none();
    }
}

static void capture_function_value_alias_state(ErrorSetCtx *ctx, FunctionValueAliasState *state) {
    if (!ctx || !state)
        return;
    state->count = ctx->function_value_alias_count;
    for (int i = 0; i < state->count; i++) {
        state->ids[i] = ctx->function_value_alias_ids[i];
        state->targets[i] = ctx->function_value_alias_targets[i];
    }
}

static void restore_function_value_alias_state(ErrorSetCtx *ctx,
                                               const FunctionValueAliasState *state) {
    if (!ctx || !state)
        return;
    ctx->function_value_alias_count = state->count;
    for (int i = 0; i < state->count; i++) {
        ctx->function_value_alias_ids[i] = state->ids[i];
        ctx->function_value_alias_targets[i] = state->targets[i];
    }
}

static bool function_value_alias_state_equal(const FunctionValueAliasState *a,
                                             const FunctionValueAliasState *b) {
    if (!a || !b || a->count != b->count)
        return false;
    for (int i = 0; i < a->count; i++) {
        if (a->ids[i] != b->ids[i] || !function_value_target_equal(a->targets[i], b->targets[i]))
            return false;
    }
    return true;
}

static FunctionValueTarget state_lookup_function_value_target(const FunctionValueAliasState *state,
                                                              uint32_t symbol_id) {
    if (!state || symbol_id == 0)
        return function_value_target_none();
    for (int i = state->count - 1; i >= 0; i--) {
        if (state->ids[i] == symbol_id)
            return state->targets[i];
    }
    return function_value_target_none();
}

static bool function_value_alias_id_present(ErrorSetCtx *ctx, uint32_t symbol_id) {
    if (!ctx || symbol_id == 0)
        return false;
    for (int i = ctx->function_value_alias_count - 1; i >= 0; i--) {
        if (ctx->function_value_alias_ids[i] == symbol_id)
            return true;
    }
    return false;
}

static FunctionExprCaptureEntry *lookup_function_expr_capture_entry(ErrorSetCtx *ctx,
                                                                    AstNode *function_expr) {
    if (!ctx || !function_expr)
        return NULL;
    for (int i = 0; i < ctx->function_expr_capture_count; i++) {
        if (ctx->function_expr_captures[i].function_expr == function_expr)
            return &ctx->function_expr_captures[i];
    }
    return NULL;
}

static void record_function_expr_capture(ErrorSetCtx *ctx, AstNode *function_expr) {
    if (!ctx || !function_expr || function_expr->type != AST_FUNCTION_EXPR)
        return;
    FunctionValueAliasState state;
    capture_function_value_alias_state(ctx, &state);

    FunctionExprCaptureEntry *entry = lookup_function_expr_capture_entry(ctx, function_expr);
    if (!entry) {
        if (ctx->function_expr_capture_count >= ctx->function_expr_capture_capacity) {
            int new_cap = ctx->function_expr_capture_capacity == 0
                              ? 32
                              : ctx->function_expr_capture_capacity * 2;
            XR_REALLOC_OR_ABORT(ctx->function_expr_captures,
                                (size_t) new_cap * sizeof(FunctionExprCaptureEntry),
                                "function expr capture grow");
            ctx->function_expr_capture_capacity = new_cap;
        }
        entry = &ctx->function_expr_captures[ctx->function_expr_capture_count++];
        memset(entry, 0, sizeof(*entry));
        entry->function_expr = function_expr;
        ctx->changed = true;
    } else if (!function_value_alias_state_equal(&entry->state, &state)) {
        ctx->changed = true;
    }
    entry->state = state;
}

static void apply_function_expr_capture(ErrorSetCtx *ctx, AstNode *function_expr) {
    FunctionExprCaptureEntry *entry = lookup_function_expr_capture_entry(ctx, function_expr);
    if (!ctx || !entry)
        return;
    for (int i = 0; i < entry->state.count; i++) {
        uint32_t id = entry->state.ids[i];
        if (id == 0 || function_value_alias_id_present(ctx, id))
            continue;
        FunctionValueTarget target = entry->state.targets[i];
        if (!function_value_target_is_exact(target))
            continue;
        XaSymbol *sym = lookup_symbol_by_id(ctx, id);
        if (!sym || !symbol_has_function_type(sym))
            continue;
        set_function_value_alias_target(ctx, sym, target);
    }
}

static void merge_function_value_path_states(ErrorSetCtx *ctx, const FunctionValueAliasState *base,
                                             const FunctionValueAliasState **path_states,
                                             int path_count) {
    if (!ctx || !base || !path_states || path_count <= 0)
        return;
    restore_function_value_alias_state(ctx, base);

    uint32_t ids[128];
    int count = 0;
    for (int s = -1; s < path_count; s++) {
        const FunctionValueAliasState *state = s < 0 ? base : path_states[s];
        if (!state)
            continue;
        for (int i = 0; i < state->count; i++) {
            uint32_t id = state->ids[i];
            if (id == 0)
                continue;
            bool seen = false;
            for (int j = 0; j < count; j++) {
                if (ids[j] == id) {
                    seen = true;
                    break;
                }
            }
            if (!seen && count < 128)
                ids[count++] = id;
        }
    }

    for (int i = 0; i < count; i++) {
        FunctionValueTarget merged = state_lookup_function_value_target(path_states[0], ids[i]);
        for (int p = 1; p < path_count && function_value_target_is_exact(merged); p++) {
            merged = function_value_target_merge(
                merged, state_lookup_function_value_target(path_states[p], ids[i]));
        }
        invalidate_function_value_alias_target(ctx, ids[i]);
        if (!function_value_target_is_exact(merged))
            continue;
        XaSymbol *sym = lookup_symbol_by_id(ctx, ids[i]);
        if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
            !symbol_has_function_type(sym))
            continue;
        set_function_value_alias_target(ctx, sym, merged);
    }
}

static void merge_function_value_if_states(ErrorSetCtx *ctx, const FunctionValueAliasState *base,
                                           const FunctionValueAliasState *then_state,
                                           const FunctionValueAliasState *else_state) {
    const FunctionValueAliasState *paths[2] = {then_state, else_state};
    merge_function_value_path_states(ctx, base, paths, 2);
}

static void merge_function_value_loop_state(ErrorSetCtx *ctx, const FunctionValueAliasState *base,
                                            const FunctionValueAliasState *iteration_state) {
    const FunctionValueAliasState *paths[2] = {base, iteration_state};
    merge_function_value_path_states(ctx, base, paths, 2);
}

static void track_function_value_alias_mutation(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || ctx->function_value_mutation_depth <= 0 || !sym || sym->id == 0 ||
        sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym))
        return;
    for (int i = 0; i < ctx->function_value_mutation_count; i++) {
        if (ctx->function_value_mutation_ids[i] == sym->id)
            return;
    }
    if (ctx->function_value_mutation_count >= 128)
        return;
    ctx->function_value_mutation_ids[ctx->function_value_mutation_count++] = sym->id;
}

static void restore_function_value_alias_state_for_catch_entry(ErrorSetCtx *ctx,
                                                               const FunctionValueAliasState *base,
                                                               const uint32_t *try_mutation_ids,
                                                               int try_mutation_count) {
    restore_function_value_alias_state(ctx, base);
    for (int i = 0; i < try_mutation_count; i++)
        invalidate_function_value_alias_target(ctx, try_mutation_ids[i]);
}

static FunctionValueTarget
resolve_returned_function_value_call_target(ErrorSetCtx *ctx, AstNode *call_expr, int depth) {
    if (!ctx || !call_expr || call_expr->type != AST_CALL_EXPR || depth >= 32)
        return function_value_target_none();
    FunctionValueTarget callee_target =
        resolve_call_target_depth(ctx, call_expr->as.call_expr.callee, depth + 1);
    if (!function_value_target_is_exact(callee_target))
        return function_value_target_none();

    FunctionValueTarget returned = function_value_target_none();
    for (int i = 0; i < callee_target.target_count; i++) {
        XaSymbol *callee_sym = callee_target.target_symbols[i];
        if (!callee_sym)
            return function_value_target_none();
        bool seen = false;
        bool unknown = false;
        FunctionValueTarget target =
            lookup_function_return_target(ctx, callee_sym, &seen, &unknown);
        if (!seen || unknown || !function_value_target_is_exact(target))
            return function_value_target_none();
        returned = function_value_target_is_exact(returned)
                       ? function_value_target_merge(returned, target)
                       : target;
        if (!function_value_target_is_exact(returned))
            return function_value_target_none();
    }
    return returned;
}

static FunctionValueTarget resolve_function_value_expr_target(ErrorSetCtx *ctx, AstNode *expr,
                                                              int depth) {
    if (!ctx || depth >= 32)
        return function_value_target_none();
    expr = identity_source(expr);
    if (!expr)
        return function_value_target_none();
    if (expr->type == AST_FUNCTION_EXPR) {
        record_function_expr_capture(ctx, expr);
        return function_value_target_expr(expr);
    }
    if (expr->type == AST_CALL_EXPR) {
        FunctionValueTarget returned =
            resolve_returned_function_value_call_target(ctx, expr, depth + 1);
        if (function_value_target_is_exact(returned))
            return returned;
        return function_value_target_none();
    }
    if (expr->type != AST_VARIABLE)
        return function_value_target_none();
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, expr);
    XaSymbol *target = resolve_function_alias_target(ctx->analyzer, sym, 0);
    if (target && (target->kind == XA_SYM_FUNCTION || target->kind == XA_SYM_METHOD))
        return function_value_target_symbol(target);
    if (sym && sym->is_const && !sym->is_rebindable && symbol_has_function_type(sym) &&
        sym->links.const_initializer) {
        FunctionValueTarget const_target =
            resolve_function_value_expr_target(ctx, sym->links.const_initializer, depth + 1);
        if (function_value_target_is_exact(const_target))
            return const_target;
    }
    FunctionValueTarget alias_target = lookup_function_value_alias_target(ctx, sym);
    if (function_value_target_is_exact(alias_target))
        return alias_target;
    return function_value_target_none();
}

static void maybe_record_function_value_var_initializer(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_VAR_DECL)
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->initializer || decl->symbol_id == 0)
        return;
    XaSymbol *sym = lookup_symbol_by_id(ctx, decl->symbol_id);
    if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym))
        return;
    FunctionValueTarget target = resolve_function_value_expr_target(ctx, decl->initializer, 0);
    if (function_value_target_is_exact(target))
        set_function_value_alias_target(ctx, sym, target);
}

static void record_function_value_assignment(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !assign)
        return;
    XaSymbol *sym = lookup_assignment_symbol(ctx, assign);
    uint32_t symbol_id = sym ? sym->id : assign->symbol_id;
    if (symbol_id == 0)
        return;
    track_function_value_alias_mutation(ctx, sym);
    FunctionValueTarget target = function_value_target_none();
    if (ctx->function_value_control_depth == 0)
        target = resolve_function_value_expr_target(ctx, assign->value, 0);

    invalidate_function_value_alias_target(ctx, symbol_id);

    if (ctx->function_value_control_depth != 0 || !function_value_target_is_exact(target))
        return;
    if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym))
        return;
    set_function_value_alias_target(ctx, sym, target);
}

/* Resolve a call target exactly when possible.  Unknown function values keep
 * their variable symbol so the dynamic-call guard can mark the summary incomplete. */
static FunctionValueTarget resolve_call_target_depth(ErrorSetCtx *ctx, AstNode *callee, int depth) {
    if (!ctx || depth >= 32)
        return function_value_target_none();
    AstNode *source = identity_source(callee);
    const XaSelection *sel = xa_analyzer_get_selection(ctx->analyzer, source);
    if (sel && sel->target_symbol &&
        (sel->kind == XA_SEL_METHOD || sel->kind == XA_SEL_STATIC_MEMBER ||
         sel->kind == XA_SEL_MODULE_EXPORT)) {
        XaSymbol *selected = sel->target_symbol;
        if (selected->kind == XA_SYM_FUNCTION || selected->kind == XA_SYM_METHOD)
            return function_value_target_symbol(selected);
    }

    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, callee);
    FunctionValueTarget target = resolve_function_value_expr_target(ctx, callee, depth + 1);
    if (function_value_target_is_exact(target))
        return target;
    return function_value_target_symbol(sym);
}

static FunctionValueTarget resolve_call_target(ErrorSetCtx *ctx, AstNode *callee) {
    return resolve_call_target_depth(ctx, callee, 0);
}

static bool is_dynamic_function_call_target(XaAnalyzer *analyzer, AstNode *callee,
                                            XaSymbol *resolved_sym) {
    if (!analyzer || !callee)
        return false;
    if (resolved_sym &&
        (resolved_sym->kind == XA_SYM_FUNCTION || resolved_sym->kind == XA_SYM_METHOD))
        return false;
    XrType *callee_type = xa_analyzer_get_node_type(analyzer, callee);
    return callee_type && XR_TYPE_IS_FUNCTION(callee_type);
}

static bool es_walk_function_expr_body(ErrorSetCtx *ctx, AstNode *function_expr) {
    if (!ctx || !function_expr || function_expr->type != AST_FUNCTION_EXPR)
        return false;
    FunctionDeclNode *fn = &function_expr->as.function_expr;
    if (!fn->body)
        return true;

    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, function_expr);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;
    FunctionValueAliasState saved_alias_state;
    capture_function_value_alias_state(ctx, &saved_alias_state);
    apply_function_expr_capture(ctx, function_expr);
    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    es_walk_block(ctx, fn->body);
    restore_function_value_alias_state(ctx, &saved_alias_state);
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    ctx->current_func = saved_func;
    ctx->analyzer->current_scope = saved_scope;
    return true;
}

static bool es_walk_immediate_function_expr_call(ErrorSetCtx *ctx, AstNode *callee) {
    AstNode *source = identity_source(callee);
    if (!source || source->type != AST_FUNCTION_EXPR)
        return false;
    return es_walk_function_expr_body(ctx, source);
}

static int function_like_param_count(AstNode *node) {
    if (!node)
        return 0;
    if (node->type == AST_FUNCTION_DECL)
        return node->as.function_decl.param_count;
    if (node->type == AST_METHOD_DECL)
        return node->as.method_decl.param_count;
    return 0;
}

static XaSymbol *function_like_param_symbol(ErrorSetCtx *ctx, AstNode *node, XaScope *fn_scope,
                                            int index) {
    if (!ctx || !node || index < 0)
        return NULL;
    if (node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *fn = &node->as.function_decl;
        if (!fn->params || index >= fn->param_count || !fn->params[index] ||
            fn->params[index]->symbol_id == 0)
            return NULL;
        return lookup_symbol_by_id(ctx, fn->params[index]->symbol_id);
    }
    if (node->type == AST_METHOD_DECL) {
        MethodDeclNode *md = &node->as.method_decl;
        const char *name =
            (md->parameters && index < md->param_count) ? md->parameters[index] : NULL;
        return name && fn_scope ? xa_scope_lookup_local(fn_scope, name) : NULL;
    }
    return NULL;
}

static bool es_walk_callsite_function_decl_body(ErrorSetCtx *ctx, XaSymbol *callee_sym,
                                                const CallExprNode *call) {
    if (!ctx || !callee_sym || !call ||
        (callee_sym->kind != XA_SYM_FUNCTION && callee_sym->kind != XA_SYM_METHOD) ||
        ctx->callsite_inline_depth >= 8)
        return false;
    AstNode *fn_node = callee_sym->links.function_decl_node;
    if (!fn_node || (fn_node->type != AST_FUNCTION_DECL && fn_node->type != AST_METHOD_DECL))
        return false;
    AstNode *body = function_like_body(fn_node);
    int param_count = function_like_param_count(fn_node);
    if (!body || param_count <= 0 || call->arg_count <= 0)
        return false;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, fn_node);

    FunctionValueAliasState saved_alias_state;
    capture_function_value_alias_state(ctx, &saved_alias_state);

    int bound_count = 0;
    int n = param_count < call->arg_count ? param_count : call->arg_count;
    for (int i = 0; i < n; i++) {
        AstNode *arg = call->arguments ? call->arguments[i] : NULL;
        if (!arg)
            continue;
        XaSymbol *param_sym = function_like_param_symbol(ctx, fn_node, fn_scope, i);
        if (!param_sym || !symbol_has_function_type(param_sym))
            continue;
        FunctionValueTarget arg_target = resolve_function_value_expr_target(ctx, arg, 0);
        if (!function_value_target_is_exact(arg_target))
            continue;
        set_function_value_alias_target(ctx, param_sym, arg_target);
        bound_count++;
    }

    if (bound_count == 0) {
        restore_function_value_alias_state(ctx, &saved_alias_state);
        return false;
    }

    XaScope *saved_scope = ctx->analyzer->current_scope;
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    XaSymbol *saved_func = ctx->current_func;
    FunctionValueTarget saved_return_target = ctx->current_return_target;
    bool saved_return_seen = ctx->current_return_target_seen;
    bool saved_return_unknown = ctx->current_return_target_unknown;

    ctx->current_func = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    ctx->callsite_inline_depth++;
    es_walk_block(ctx, body);
    ctx->callsite_inline_depth--;

    restore_function_value_alias_state(ctx, &saved_alias_state);
    ctx->current_return_target = saved_return_target;
    ctx->current_return_target_seen = saved_return_seen;
    ctx->current_return_target_unknown = saved_return_unknown;
    ctx->current_func = saved_func;
    ctx->analyzer->current_scope = saved_scope;
    return true;
}

static bool is_current_caught_ref(ErrorSetCtx *ctx, AstNode *expr) {
    expr = identity_source(expr);
    if (!ctx || !expr || expr->type != AST_VARIABLE || !expr->as.variable.name ||
        !ctx->current_caught)
        return false;

    uint32_t symbol_id = expr->as.variable.symbol_id;
    if (symbol_id != 0) {
        if (ctx->current_catch_binding_is_caught && ctx->current_catch_symbol_id != 0 &&
            symbol_id == ctx->current_catch_symbol_id)
            return true;
        for (int i = 0; i < ctx->current_catch_alias_count; i++) {
            if (ctx->current_catch_alias_ids[i] != 0 &&
                symbol_id == ctx->current_catch_alias_ids[i])
                return true;
        }
        return ctx->current_catch_binding_is_caught && ctx->current_catch_var &&
               strcmp(expr->as.variable.name, ctx->current_catch_var) == 0;
    }

    if (ctx->current_catch_binding_is_caught && ctx->current_catch_var &&
        strcmp(expr->as.variable.name, ctx->current_catch_var) == 0)
        return true;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        const char *name = ctx->current_catch_alias_names[i];
        if (name && strcmp(expr->as.variable.name, name) == 0)
            return true;
    }
    return false;
}

static bool catch_symbol_matches(uint32_t lhs_id, const char *lhs_name, uint32_t rhs_id,
                                 const char *rhs_name) {
    if (lhs_id != 0 && rhs_id != 0)
        return lhs_id == rhs_id;
    return lhs_name && rhs_name && strcmp(lhs_name, rhs_name) == 0;
}

static bool current_catch_target_matches(ErrorSetCtx *ctx, uint32_t symbol_id, const char *name) {
    if (!ctx)
        return false;
    if (symbol_id != 0 && ctx->current_catch_symbol_id != 0 &&
        symbol_id == ctx->current_catch_symbol_id)
        return true;
    return name && ctx->current_catch_var && strcmp(name, ctx->current_catch_var) == 0;
}

static int current_catch_alias_index(ErrorSetCtx *ctx, uint32_t symbol_id, const char *name) {
    if (!ctx)
        return -1;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        if (catch_symbol_matches(symbol_id, name, ctx->current_catch_alias_ids[i],
                                 ctx->current_catch_alias_names[i]))
            return i;
    }
    return -1;
}

static void capture_catch_alias_state(ErrorSetCtx *ctx, CatchAliasState *state) {
    if (!ctx || !state)
        return;
    state->binding_is_caught = ctx->current_catch_binding_is_caught;
    state->alias_count = ctx->current_catch_alias_count;
    if (state->alias_count > 64)
        state->alias_count = 64;
    for (int i = 0; i < state->alias_count; i++) {
        state->alias_ids[i] = ctx->current_catch_alias_ids[i];
        state->alias_names[i] = ctx->current_catch_alias_names[i];
    }
}

static void restore_catch_alias_state(ErrorSetCtx *ctx, const CatchAliasState *state) {
    if (!ctx || !state)
        return;
    ctx->current_catch_binding_is_caught = state->binding_is_caught;
    ctx->current_catch_alias_count = state->alias_count;
    if (ctx->current_catch_alias_count > 64)
        ctx->current_catch_alias_count = 64;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        ctx->current_catch_alias_ids[i] = state->alias_ids[i];
        ctx->current_catch_alias_names[i] = state->alias_names[i];
    }
}

static bool catch_alias_state_has(const CatchAliasState *state, uint32_t symbol_id,
                                  const char *name) {
    if (!state)
        return false;
    for (int i = 0; i < state->alias_count; i++) {
        if (catch_symbol_matches(symbol_id, name, state->alias_ids[i], state->alias_names[i]))
            return true;
    }
    return false;
}

static void merge_catch_alias_intersection_states(ErrorSetCtx *ctx, const CatchAliasState *left,
                                                  const CatchAliasState *right) {
    if (!ctx || !left || !right)
        return;
    ctx->current_catch_binding_is_caught = left->binding_is_caught && right->binding_is_caught;

    uint32_t merged_ids[64];
    const char *merged_names[64];
    int merged_count = 0;
    for (int i = 0; i < left->alias_count && merged_count < 64; i++) {
        uint32_t id = left->alias_ids[i];
        const char *name = left->alias_names[i];
        if (!catch_alias_state_has(right, id, name))
            continue;
        merged_ids[merged_count] = id;
        merged_names[merged_count] = name;
        merged_count++;
    }

    ctx->current_catch_alias_count = merged_count;
    for (int i = 0; i < merged_count; i++) {
        ctx->current_catch_alias_ids[i] = merged_ids[i];
        ctx->current_catch_alias_names[i] = merged_names[i];
    }
}

static void remove_current_catch_alias(ErrorSetCtx *ctx, int index) {
    if (!ctx || index < 0 || index >= ctx->current_catch_alias_count)
        return;
    for (int i = index; i + 1 < ctx->current_catch_alias_count; i++) {
        ctx->current_catch_alias_ids[i] = ctx->current_catch_alias_ids[i + 1];
        ctx->current_catch_alias_names[i] = ctx->current_catch_alias_names[i + 1];
    }
    ctx->current_catch_alias_count--;
}

static void add_current_catch_alias(ErrorSetCtx *ctx, uint32_t symbol_id, const char *name) {
    if (!ctx || symbol_id == 0 || !name || ctx->current_catch_alias_count >= 64)
        return;
    if (current_catch_alias_index(ctx, symbol_id, name) >= 0)
        return;
    int slot = ctx->current_catch_alias_count++;
    ctx->current_catch_alias_ids[slot] = symbol_id;
    ctx->current_catch_alias_names[slot] = name;
}

static bool catch_alias_declared_in_block(ErrorSetCtx *ctx, uint32_t symbol_id, AstNode *block) {
    if (!ctx || !block || block->type != AST_BLOCK || symbol_id == 0)
        return true;
    XaScope *block_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, block);
    XaSymbol *sym = lookup_symbol_by_id(ctx, symbol_id);
    return !block_scope || !sym || sym->scope == block_scope;
}

static void finish_catch_alias_block(ErrorSetCtx *ctx, AstNode *block, int saved_count) {
    if (!ctx)
        return;
    if (!ctx->current_caught || ctx->current_catch_alias_control_depth != 0) {
        ctx->current_catch_alias_count = saved_count;
        return;
    }

    uint32_t final_ids[64];
    const char *final_names[64];
    int final_count = 0;
    for (int i = 0; i < ctx->current_catch_alias_count && final_count < 64; i++) {
        uint32_t id = ctx->current_catch_alias_ids[i];
        const char *name = ctx->current_catch_alias_names[i];
        if (catch_alias_declared_in_block(ctx, id, block))
            continue;
        final_ids[final_count] = id;
        final_names[final_count] = name;
        final_count++;
    }
    ctx->current_catch_alias_count = final_count;
    for (int i = 0; i < final_count; i++) {
        ctx->current_catch_alias_ids[i] = final_ids[i];
        ctx->current_catch_alias_names[i] = final_names[i];
    }
}

static void maybe_record_catch_alias(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || !ctx->current_caught ||
        (node->type != AST_CONST_DECL && node->type != AST_VAR_DECL))
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->name || !decl->initializer || decl->symbol_id == 0 ||
        ctx->current_catch_alias_count >= 64)
        return;
    if (node->type == AST_VAR_DECL) {
        XaSymbol *sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, decl->symbol_id);
        if (!sym || sym->kind != XA_SYM_VARIABLE || sym->links.assign_count != 1)
            return;
    }
    if (!is_current_caught_ref(ctx, decl->initializer))
        return;
    add_current_catch_alias(ctx, decl->symbol_id, decl->name);
}

static void record_catch_alias_assignment(ErrorSetCtx *ctx, AssignmentNode *assign) {
    if (!ctx || !assign || !ctx->current_caught)
        return;
    XaSymbol *sym = lookup_assignment_symbol(ctx, assign);
    uint32_t symbol_id = sym ? sym->id : assign->symbol_id;
    const char *name = assign->name;
    if (symbol_id == 0 && !name)
        return;

    bool rhs_is_caught = is_current_caught_ref(ctx, assign->value);
    if (current_catch_target_matches(ctx, symbol_id, name)) {
        ctx->current_catch_binding_is_caught = rhs_is_caught;
        return;
    }

    int alias_index = current_catch_alias_index(ctx, symbol_id, name);
    if (alias_index < 0) {
        if (rhs_is_caught && sym && sym->kind == XA_SYM_VARIABLE && !sym->is_const &&
            sym->is_rebindable)
            add_current_catch_alias(ctx, symbol_id, name);
        return;
    }
    remove_current_catch_alias(ctx, alias_index);
    if (rhs_is_caught)
        add_current_catch_alias(ctx, symbol_id, name);
}

/* Try to find the case index of an enum member.
 * Searches the enum symbol's member list. */
static int find_enum_case_index(XaSymbol *enum_sym, const char *case_name) {
    if (!enum_sym || !case_name)
        return -1;
    XaSymbolLinks *links = &enum_sym->links;
    return xa_enum_info_find_variant(links->enum_info, case_name);
}

static XaSymbol *lookup_enum_symbol(XaAnalyzer *analyzer, const char *enum_name) {
    if (!analyzer || !enum_name)
        return NULL;
    XaSymbol *sym = xa_analyzer_lookup(analyzer, enum_name);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_in_scope(analyzer, enum_name, analyzer->global_scope);
    if (!sym || sym->kind != XA_SYM_ENUM)
        sym = xa_analyzer_lookup_deep(analyzer, enum_name);
    return (sym && sym->kind == XA_SYM_ENUM) ? sym : NULL;
}

/* ========== Expression Walking ========== */

static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node) {
    if (!node)
        return;

    switch (node->type) {
        case AST_CALL_EXPR: {
            /* Walk arguments first */
            for (int i = 0; i < node->as.call_expr.arg_count; i++) {
                es_walk_expr(ctx, node->as.call_expr.arguments[i]);
            }
            es_walk_expr(ctx, node->as.call_expr.callee);

            if (es_walk_immediate_function_expr_call(ctx, node->as.call_expr.callee))
                break;

            /* Union callee's effect summary into current function's summary */
            FunctionValueTarget call_target = resolve_call_target(ctx, node->as.call_expr.callee);
            if (function_value_target_is_exact(call_target)) {
                for (int i = 0; i < call_target.target_count; i++) {
                    AstNode *function_expr = call_target.target_function_exprs[i];
                    XaSymbol *callee_sym = call_target.target_symbols[i];
                    if (function_expr) {
                        es_walk_function_expr_body(ctx, function_expr);
                        continue;
                    }
                    if (es_walk_callsite_function_decl_body(ctx, callee_sym, &node->as.call_expr))
                        continue;
                    if (callee_sym &&
                        (callee_sym->kind == XA_SYM_FUNCTION ||
                         callee_sym->kind == XA_SYM_METHOD) &&
                        callee_sym->links.effect_id != XA_EFFECT_NONE) {
                        const XaEffectSummary *callee_summary =
                            xa_effect_db_get(ctx->analyzer->effect_db, callee_sym->links.effect_id);
                        if (callee_summary)
                            xa_effect_summary_add_summary(ctx->analyzer->effect_db,
                                                          ctx->current_summary, callee_summary);
                    }
                }
                break;
            }
            if (is_dynamic_function_call_target(ctx->analyzer, node->as.call_expr.callee,
                                                call_target.symbol)) {
                xa_effect_summary_mark_incomplete(ctx->current_summary,
                                                  XA_UNKNOWN_DYNAMIC_CALL_TARGET);
            }
            break;
        }

        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
            es_walk_expr(ctx, node->as.binary.left);
            es_walk_expr(ctx, node->as.binary.right);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            es_walk_expr(ctx, node->as.unary.operand);
            break;

        case AST_TERNARY:
            es_walk_expr(ctx, node->as.ternary.condition);
            es_walk_expr(ctx, node->as.ternary.true_expr);
            es_walk_expr(ctx, node->as.ternary.false_expr);
            break;

        case AST_MEMBER_ACCESS:
            es_walk_expr(ctx, node->as.member_access.object);
            break;

        case AST_INDEX_GET:
            es_walk_expr(ctx, node->as.index_get.array);
            es_walk_expr(ctx, node->as.index_get.index);
            break;

        case AST_ARRAY_LITERAL:
            if (node->as.array_literal.is_repeat) {
                es_walk_expr(ctx, node->as.array_literal.repeat_value);
                es_walk_expr(ctx, node->as.array_literal.repeat_count);
            } else {
                for (int i = 0; i < node->as.array_literal.count; i++)
                    es_walk_expr(ctx, node->as.array_literal.elements[i]);
            }
            break;

        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                es_walk_expr(ctx, node->as.template_str.parts[i]);
            break;

        case AST_MATCH_EXPR:
            es_walk_expr(ctx, node->as.match_expr.expr);
            for (int i = 0; i < node->as.match_expr.arm_count; i++) {
                AstNode *arm = node->as.match_expr.arms[i];
                if (arm)
                    es_walk_expr(ctx, arm->as.match_arm.body);
            }
            break;

        case AST_NULLISH_COALESCE:
            es_walk_expr(ctx, node->as.binary.left);
            es_walk_expr(ctx, node->as.binary.right);
            break;

        case AST_FUNCTION_EXPR:
            /* Lambda: don't propagate its errors to the enclosing function. */
            break;

        case AST_ASSIGNMENT:
            es_walk_expr(ctx, node->as.assignment.value);
            record_catch_alias_assignment(ctx, &node->as.assignment);
            record_function_value_assignment(ctx, &node->as.assignment);
            break;

        case AST_GROUPING:
            es_walk_expr(ctx, node->as.grouping);
            break;

        default:
            break;
    }
}

/* ========== Statement Walking ========== */

static void es_walk_block(ErrorSetCtx *ctx, AstNode *node) {
    if (!node)
        return;
    if (node->type == AST_BLOCK) {
        int saved_catch_alias_count = ctx->current_catch_alias_count;
        int saved_function_value_alias_count = ctx->function_value_alias_count;
        for (int i = 0; i < node->as.block.count; i++)
            es_walk_stmt(ctx, node->as.block.statements[i]);
        finish_catch_alias_block(ctx, node, saved_catch_alias_count);
        ctx->function_value_alias_count = saved_function_value_alias_count;
    } else {
        es_walk_stmt(ctx, node);
    }
}

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node) {
    if (!node)
        return;

    switch (node->type) {
        case AST_THROW_STMT: {
            AstNode *expr = node->as.throw_stmt.expression;
            if (!expr)
                break;

            es_walk_expr(ctx, expr);
            if (is_current_caught_ref(ctx, expr)) {
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, ctx->current_summary,
                                              ctx->current_caught);
                break;
            }

            const XaSelection *throw_sel = xa_analyzer_get_selection(ctx->analyzer, expr);
            if (es_summary_add_enum_selection(ctx, throw_sel))
                break;
            if (expr->type == AST_CALL_EXPR &&
                es_summary_add_enum_selection(
                    ctx, xa_analyzer_get_selection(ctx->analyzer, expr->as.call_expr.callee)))
                break;

            /*
             * Handle `throw EnumName.CaseName` — add the specific case.
             * Handle `throw variable` where variable has enum type — add all cases.
             */
            if (expr->type == AST_ENUM_ACCESS) {
                const char *enum_name = expr->as.enum_access.enum_name;
                const char *member_name = expr->as.enum_access.member_name;
                const XaSelection *sel = xa_analyzer_get_selection(ctx->analyzer, expr);
                XaSymbol *enum_sym = NULL;
                XrType *enum_type = NULL;
                int case_idx = -1;
                if (sel && sel->kind == XA_SEL_ENUM_MEMBER) {
                    enum_sym = sel->target_symbol;
                    enum_type = sel->result_type;
                    case_idx = sel->field_index;
                }
                if (!enum_sym && enum_name)
                    enum_sym = lookup_enum_symbol(ctx->analyzer, enum_name);
                if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
                    if (!enum_type)
                        enum_type = enum_sym->links.type;
                    if (case_idx < 0)
                        case_idx = find_enum_case_index(enum_sym, member_name);
                    if (enum_type && case_idx >= 0) {
                        es_summary_add_enum_case(ctx->analyzer->effect_db, ctx->current_summary,
                                                 enum_type, (uint32_t) case_idx);
                    } else if (enum_type) {
                        es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary,
                                                enum_type);
                    }
                }
            } else {
                /* Generic throw: infer type from the expression's analyzed type */
                XrType *thrown_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
                if (thrown_type && XR_TYPE_IS_ENUM(thrown_type)) {
                    es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary,
                                            thrown_type);
                }
            }
            break;
        }

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;

            XaEffectSummary try_summary;
            xa_effect_summary_init(&try_summary);
            XaEffectSummary *outer_summary = ctx->current_summary;
            bool exact_function_values = ctx->function_value_control_depth == 0;
            FunctionValueAliasState base_state;
            FunctionValueAliasState try_state;
            FunctionValueAliasState *catch_states = NULL;
            uint32_t try_mutation_ids[128];
            int try_mutation_count = 0;

            ctx->current_summary = &try_summary;
            if (exact_function_values) {
                int mutation_start = ctx->function_value_mutation_count;
                int mutation_depth_before = ctx->function_value_mutation_depth;

                capture_function_value_alias_state(ctx, &base_state);
                restore_function_value_alias_state(ctx, &base_state);
                ctx->function_value_mutation_depth++;
                es_walk_block(ctx, tc->try_body);
                ctx->function_value_mutation_depth--;
                capture_function_value_alias_state(ctx, &try_state);

                for (int i = mutation_start; i < ctx->function_value_mutation_count; i++) {
                    uint32_t id = ctx->function_value_mutation_ids[i];
                    if (id == 0)
                        continue;
                    bool seen = false;
                    for (int j = 0; j < try_mutation_count; j++) {
                        if (try_mutation_ids[j] == id) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen && try_mutation_count < 128)
                        try_mutation_ids[try_mutation_count++] = id;
                }
                if (mutation_depth_before == 0)
                    ctx->function_value_mutation_count = mutation_start;

                if (tc->catch_count > 0)
                    catch_states = (FunctionValueAliasState *) xr_calloc(
                        (size_t) tc->catch_count, sizeof(FunctionValueAliasState));
            } else {
                ctx->function_value_control_depth++;
                es_walk_block(ctx, tc->try_body);
                ctx->function_value_control_depth--;
            }
            ctx->current_summary = outer_summary;

            if (tc->catch_count > 0) {
                XaEffectSummary *caught_summaries = (XaEffectSummary *) xr_calloc(
                    (size_t) tc->catch_count, sizeof(XaEffectSummary));
                if (caught_summaries) {
                    for (int i = 0; i < tc->catch_count; i++)
                        xa_effect_summary_init(&caught_summaries[i]);
                }
                for (int i = 0; i < tc->catch_count; i++) {
                    XrCatchClause *cc = tc->catch_clauses[i];
                    if (!cc || cc->is_panic)
                        continue;
                    if (!cc->type) {
                        if (caught_summaries)
                            xa_effect_summary_add_summary(ctx->analyzer->effect_db,
                                                          &caught_summaries[i], &try_summary);
                        xa_effect_summary_clear_escaping(&try_summary);
                        continue;
                    }
                    XrType *catch_type = xr_tref_resolve_in_analyzer(ctx->analyzer, cc->type);
                    if (catch_type && XR_TYPE_IS_ENUM(catch_type)) {
                        XaErrorTypeId type_id =
                            xa_effect_db_register_error_enum(ctx->analyzer->effect_db, catch_type);
                        if (caught_summaries)
                            xa_effect_summary_add_type_from_summary(ctx->analyzer->effect_db,
                                                                    &caught_summaries[i],
                                                                    &try_summary, type_id);
                        xa_effect_summary_subtract_type(&try_summary, type_id);
                    }
                }
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, outer_summary,
                                              &try_summary);
                for (int i = 0; i < tc->catch_count; i++) {
                    XrCatchClause *cc = tc->catch_clauses[i];
                    if (cc && cc->body) {
                        const char *saved_catch_var = ctx->current_catch_var;
                        uint32_t saved_catch_symbol_id = ctx->current_catch_symbol_id;
                        bool saved_catch_binding_is_caught = ctx->current_catch_binding_is_caught;
                        int saved_catch_alias_count = ctx->current_catch_alias_count;
                        XaEffectSummary *saved_caught = ctx->current_caught;
                        ctx->current_catch_var = cc->var_name;
                        ctx->current_catch_symbol_id = cc->symbol_id;
                        ctx->current_catch_binding_is_caught = true;
                        ctx->current_catch_alias_count = 0;
                        ctx->current_caught = caught_summaries ? &caught_summaries[i] : NULL;
                        if (exact_function_values)
                            restore_function_value_alias_state_for_catch_entry(
                                ctx, &base_state, try_mutation_ids, try_mutation_count);
                        else
                            ctx->function_value_control_depth++;
                        es_walk_block(ctx, cc->body);
                        if (exact_function_values && catch_states)
                            capture_function_value_alias_state(ctx, &catch_states[i]);
                        if (!exact_function_values)
                            ctx->function_value_control_depth--;
                        ctx->current_catch_var = saved_catch_var;
                        ctx->current_catch_symbol_id = saved_catch_symbol_id;
                        ctx->current_catch_binding_is_caught = saved_catch_binding_is_caught;
                        ctx->current_catch_alias_count = saved_catch_alias_count;
                        ctx->current_caught = saved_caught;
                    } else if (exact_function_values && catch_states) {
                        restore_function_value_alias_state_for_catch_entry(
                            ctx, &base_state, try_mutation_ids, try_mutation_count);
                        capture_function_value_alias_state(ctx, &catch_states[i]);
                    }
                }
                if (caught_summaries) {
                    for (int i = 0; i < tc->catch_count; i++)
                        xa_effect_summary_clear(&caught_summaries[i]);
                    xr_free(caught_summaries);
                }
            } else {
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, outer_summary,
                                              &try_summary);
            }
            if (exact_function_values) {
                if (tc->catch_count > 0 && catch_states) {
                    const FunctionValueAliasState *paths[129];
                    int path_count = 0;
                    paths[path_count++] = &try_state;
                    for (int i = 0; i < tc->catch_count && path_count < 129; i++)
                        paths[path_count++] = &catch_states[i];
                    merge_function_value_path_states(ctx, &base_state, paths, path_count);
                } else {
                    restore_function_value_alias_state(ctx, &try_state);
                }
            }
            if (catch_states)
                xr_free(catch_states);
            xa_effect_summary_clear(&try_summary);
            break;
        }

        case AST_EXPR_STMT:
            es_walk_expr(ctx, node->as.expr_stmt);
            break;

        case AST_VAR_DECL:
            maybe_record_catch_alias(ctx, node);
            maybe_record_function_value_var_initializer(ctx, node);
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_CONST_DECL:
            maybe_record_catch_alias(ctx, node);
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;
        case AST_SHARED_DECL:
        case AST_OWNED_DECL:
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_ASSIGNMENT:
            es_walk_expr(ctx, node->as.assignment.value);
            record_catch_alias_assignment(ctx, &node->as.assignment);
            record_function_value_assignment(ctx, &node->as.assignment);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                es_walk_expr(ctx, node->as.return_stmt.values[i]);
            if (ctx->current_func) {
                if (node->as.return_stmt.value_count == 1) {
                    AstNode *value = node->as.return_stmt.values[0];
                    FunctionValueTarget target = resolve_function_value_expr_target(ctx, value, 0);
                    if (function_value_target_is_exact(target)) {
                        record_current_function_return_target(ctx, target, false);
                    } else if (expr_has_function_type(ctx, value) ||
                               current_function_returns_function(ctx)) {
                        record_current_function_return_target(ctx, function_value_target_none(),
                                                              true);
                    }
                } else if (current_function_returns_function(ctx)) {
                    record_current_function_return_target(ctx, function_value_target_none(), true);
                }
            }
            break;

        case AST_IF_STMT:
            es_walk_expr(ctx, node->as.if_stmt.condition);
            bool merge_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState catch_base_state;
            CatchAliasState catch_then_state;
            CatchAliasState catch_else_state;
            if (merge_catch_aliases)
                capture_catch_alias_state(ctx, &catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                es_walk_block(ctx, node->as.if_stmt.then_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_then_state);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                es_walk_block(ctx, node->as.if_stmt.else_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_else_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState then_state;
                FunctionValueAliasState else_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                es_walk_block(ctx, node->as.if_stmt.then_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_then_state);
                capture_function_value_alias_state(ctx, &then_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_catch_aliases)
                    restore_catch_alias_state(ctx, &catch_base_state);
                if (node->as.if_stmt.else_branch)
                    es_walk_block(ctx, node->as.if_stmt.else_branch);
                if (merge_catch_aliases)
                    capture_catch_alias_state(ctx, &catch_else_state);
                capture_function_value_alias_state(ctx, &else_state);

                merge_function_value_if_states(ctx, &base_state, &then_state, &else_state);
            }
            if (merge_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &catch_then_state, &catch_else_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_WHILE_STMT:
            es_walk_expr(ctx, node->as.while_stmt.condition);
            bool merge_while_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState while_catch_base_state;
            CatchAliasState while_catch_iteration_state;
            if (merge_while_catch_aliases)
                capture_catch_alias_state(ctx, &while_catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_while_catch_aliases)
                    restore_catch_alias_state(ctx, &while_catch_base_state);
                es_walk_block(ctx, node->as.while_stmt.body);
                if (merge_while_catch_aliases)
                    capture_catch_alias_state(ctx, &while_catch_iteration_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState iteration_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_while_catch_aliases)
                    restore_catch_alias_state(ctx, &while_catch_base_state);
                es_walk_block(ctx, node->as.while_stmt.body);
                if (merge_while_catch_aliases)
                    capture_catch_alias_state(ctx, &while_catch_iteration_state);
                capture_function_value_alias_state(ctx, &iteration_state);

                merge_function_value_loop_state(ctx, &base_state, &iteration_state);
            }
            if (merge_while_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &while_catch_base_state,
                                                      &while_catch_iteration_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_FOR_STMT:
            es_walk_stmt(ctx, node->as.for_stmt.initializer);
            es_walk_expr(ctx, node->as.for_stmt.condition);
            bool merge_for_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState for_catch_base_state;
            CatchAliasState for_catch_iteration_state;
            if (merge_for_catch_aliases)
                capture_catch_alias_state(ctx, &for_catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_for_catch_aliases)
                    restore_catch_alias_state(ctx, &for_catch_base_state);
                es_walk_block(ctx, node->as.for_stmt.body);
                es_walk_expr(ctx, node->as.for_stmt.increment);
                if (merge_for_catch_aliases)
                    capture_catch_alias_state(ctx, &for_catch_iteration_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState iteration_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_for_catch_aliases)
                    restore_catch_alias_state(ctx, &for_catch_base_state);
                es_walk_block(ctx, node->as.for_stmt.body);
                es_walk_expr(ctx, node->as.for_stmt.increment);
                if (merge_for_catch_aliases)
                    capture_catch_alias_state(ctx, &for_catch_iteration_state);
                capture_function_value_alias_state(ctx, &iteration_state);

                merge_function_value_loop_state(ctx, &base_state, &iteration_state);
            }
            if (merge_for_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &for_catch_base_state,
                                                      &for_catch_iteration_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_FOR_IN_STMT:
            es_walk_expr(ctx, node->as.for_in_stmt.collection);
            bool merge_for_in_catch_aliases =
                ctx->current_caught && ctx->current_catch_alias_control_depth == 0;
            CatchAliasState for_in_catch_base_state;
            CatchAliasState for_in_catch_iteration_state;
            if (merge_for_in_catch_aliases)
                capture_catch_alias_state(ctx, &for_in_catch_base_state);
            else
                ctx->current_catch_alias_control_depth++;
            if (ctx->function_value_control_depth != 0) {
                ctx->function_value_control_depth++;
                if (merge_for_in_catch_aliases)
                    restore_catch_alias_state(ctx, &for_in_catch_base_state);
                es_walk_block(ctx, node->as.for_in_stmt.body);
                if (merge_for_in_catch_aliases)
                    capture_catch_alias_state(ctx, &for_in_catch_iteration_state);
                ctx->function_value_control_depth--;
            } else {
                FunctionValueAliasState base_state;
                FunctionValueAliasState iteration_state;
                capture_function_value_alias_state(ctx, &base_state);

                restore_function_value_alias_state(ctx, &base_state);
                if (merge_for_in_catch_aliases)
                    restore_catch_alias_state(ctx, &for_in_catch_base_state);
                es_walk_block(ctx, node->as.for_in_stmt.body);
                if (merge_for_in_catch_aliases)
                    capture_catch_alias_state(ctx, &for_in_catch_iteration_state);
                capture_function_value_alias_state(ctx, &iteration_state);

                merge_function_value_loop_state(ctx, &base_state, &iteration_state);
            }
            if (merge_for_in_catch_aliases)
                merge_catch_alias_intersection_states(ctx, &for_in_catch_base_state,
                                                      &for_in_catch_iteration_state);
            else
                ctx->current_catch_alias_control_depth--;
            break;

        case AST_BLOCK:
            es_walk_block(ctx, node);
            break;

        case AST_PRINT_STMT:
            for (int i = 0; i < node->as.print_stmt.expr_count; i++)
                es_walk_expr(ctx, node->as.print_stmt.exprs[i]);
            break;

        case AST_DEFER_STMT:
            es_walk_expr(ctx, node->as.defer_stmt.expr);
            break;

        case AST_FUNCTION_DECL:
            /* Nested function: analyzed separately, skip */
            break;

        default:
            break;
    }
}

/* ========== Per-Function Inference ========== */

static void infer_function_error_set(ErrorSetCtx *ctx, AstNode *func_node, XaSymbol *func_sym) {
    if (!func_node || !func_sym)
        return;

    AstNode *body = function_like_body(func_node);
    if (!body)
        return;

    ctx->current_func = func_sym;
    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, func_node);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;

    XaEffectSummary summary;
    xa_effect_summary_init(&summary);
    ctx->current_summary = &summary;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    es_walk_block(ctx, body);
    XaEffectId previous_id = func_sym->links.effect_id;
    func_sym->links.effect_id = xa_effect_db_intern(ctx->analyzer->effect_db, &summary);
    if (func_sym->links.effect_id != previous_id)
        ctx->changed = true;
    store_function_return_target(ctx, func_sym);
    xa_effect_summary_clear(&summary);

    ctx->current_summary = NULL;
    ctx->current_return_target = function_value_target_none();
    ctx->current_return_target_seen = false;
    ctx->current_return_target_unknown = false;
    ctx->analyzer->current_scope = saved_scope;
    ctx->current_func = NULL;
}

/* ========== Top-Level Collector ========== */

typedef struct FuncEntry {
    AstNode *node;
    XaSymbol *sym;
} FuncEntry;

static void collect_functions(XaAnalyzer *analyzer, AstNode *node, FuncEntry **out, int *count,
                              int *cap) {
    if (!node)
        return;

    if (node->type == AST_FUNCTION_DECL || node->type == AST_METHOD_DECL) {
        XaSymbol *sym = resolve_func_symbol(analyzer, node);
        if (sym) {
            if (*count >= *cap) {
                int new_cap = *cap == 0 ? 32 : *cap * 2;
                FuncEntry *new_arr = (FuncEntry *) xr_malloc((size_t) new_cap * sizeof(FuncEntry));
                if (*out && *count > 0)
                    memcpy(new_arr, *out, (size_t) (*count) * sizeof(FuncEntry));
                xr_free(*out);
                *out = new_arr;
                *cap = new_cap;
            }
            (*out)[*count].node = node;
            (*out)[*count].sym = sym;
            (*count)++;
        }
    }

    if (node->type == AST_PROGRAM) {
        for (int i = 0; i < node->as.program.count; i++)
            collect_functions(analyzer, node->as.program.statements[i], out, count, cap);
    }

    if (node->type == AST_CLASS_DECL) {
        for (int i = 0; i < node->as.class_decl.method_count; i++)
            collect_functions(analyzer, node->as.class_decl.methods[i], out, count, cap);
    }
}

/* ========== Public Entry Point ========== */

void xa_infer_error_sets(XaAnalyzer *analyzer, AstNode *ast) {
    if (!analyzer || !ast)
        return;

    ErrorSetCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.analyzer = analyzer;

    /* Phase 1: Collect all function declarations */
    FuncEntry *funcs = NULL;
    int func_count = 0, func_cap = 0;
    collect_functions(analyzer, ast, &funcs, &func_count, &func_cap);

    if (func_count == 0)
        goto cleanup;

    /* Phase 2: Fixpoint iteration (handles recursion)
     * Max iterations = func_count + 1 to guarantee convergence. */
    int max_iter = func_count + 1;
    for (int iter = 0; iter < max_iter; iter++) {
        ctx.changed = false;

        for (int i = 0; i < func_count; i++) {
            infer_function_error_set(&ctx, funcs[i].node, funcs[i].sym);
        }

        if (!ctx.changed)
            break;
    }

cleanup:
    xr_free(funcs);
    xr_free(ctx.function_return_targets);
    xr_free(ctx.function_expr_captures);
}
