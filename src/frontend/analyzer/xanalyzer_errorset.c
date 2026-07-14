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

typedef struct ErrorSetCtx {
    XaAnalyzer *analyzer;
    XaEffectSummary *current_summary; /* Effect summary being built for current function */
    const char *current_catch_var;    /* Catch variable currently in scope, if any */
    uint32_t current_catch_symbol_id; /* Symbol id for current_catch_var */
    uint32_t current_catch_alias_ids[64];
    const char *current_catch_alias_names[64];
    int current_catch_alias_count;
    uint32_t function_value_alias_ids[128];
    XaSymbol *function_value_alias_targets[128];
    int function_value_alias_count;
    XaEffectSummary *current_caught; /* Effect subset caught by current catch clause */
    XaSymbol *current_func;          /* Current function symbol */
    bool changed;                    /* Fixpoint: did anything change this iteration? */
} ErrorSetCtx;

/* ========== Forward Declarations ========== */

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_block(ErrorSetCtx *ctx, AstNode *node);

/* ========== Helpers ========== */

static XaSymbol *resolve_func_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL) {
        FunctionDeclNode *fn = &node->as.function_decl;
        XaScope *scope = xa_scope_find_by_node(analyzer->global_scope, node);
        if (scope && scope->function_symbol && scope->function_symbol->kind == XA_SYM_FUNCTION)
            return scope->function_symbol;
        if (fn->symbol_id != 0) {
            XaSymbol *sym = xa_scope_lookup_by_id(analyzer->global_scope, fn->symbol_id);
            if (sym && sym->kind == XA_SYM_FUNCTION)
                return sym;
        }
        const char *name = fn->name;
        if (name)
            return xa_scope_lookup(analyzer->global_scope, name);
    }
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

static XaSymbol *lookup_function_value_alias_target(ErrorSetCtx *ctx, XaSymbol *sym) {
    if (!ctx || !sym || sym->id == 0)
        return NULL;
    for (int i = ctx->function_value_alias_count - 1; i >= 0; i--) {
        if (ctx->function_value_alias_ids[i] == sym->id)
            return ctx->function_value_alias_targets[i];
    }
    return NULL;
}

static void set_function_value_alias_target(ErrorSetCtx *ctx, XaSymbol *sym, XaSymbol *target) {
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
            ctx->function_value_alias_targets[i] = NULL;
    }
}

static XaSymbol *resolve_function_value_expr_target(ErrorSetCtx *ctx, AstNode *expr, int depth) {
    if (!ctx || depth >= 32)
        return NULL;
    expr = identity_source(expr);
    if (!expr || expr->type != AST_VARIABLE)
        return NULL;
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, expr);
    XaSymbol *target = resolve_function_alias_target(ctx->analyzer, sym, 0);
    if (target && (target->kind == XA_SYM_FUNCTION || target->kind == XA_SYM_METHOD))
        return target;
    target = lookup_function_value_alias_target(ctx, sym);
    if (target && (target->kind == XA_SYM_FUNCTION || target->kind == XA_SYM_METHOD))
        return target;
    return NULL;
}

static void maybe_record_stable_function_value_var(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || node->type != AST_VAR_DECL)
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->initializer || decl->symbol_id == 0)
        return;
    XaSymbol *sym = xa_scope_lookup_by_id(ctx->analyzer->global_scope, decl->symbol_id);
    if (!sym || sym->kind != XA_SYM_VARIABLE || sym->is_const || !sym->is_rebindable ||
        !symbol_has_function_type(sym) || sym->links.assign_count != 1)
        return;
    XaSymbol *target = resolve_function_value_expr_target(ctx, decl->initializer, 0);
    if (target)
        set_function_value_alias_target(ctx, sym, target);
}

/* Resolve a call target to its function symbol.  Direct calls and const
 * function-value aliases are exact; dynamic function values remain incomplete
 * work for the wider P2/HOF pass. */
static XaSymbol *resolve_call_target(ErrorSetCtx *ctx, AstNode *callee) {
    XaSymbol *sym = lookup_variable_symbol(ctx->analyzer, callee);
    XaSymbol *target = resolve_function_alias_target(ctx->analyzer, sym, 0);
    if (target && (target->kind == XA_SYM_FUNCTION || target->kind == XA_SYM_METHOD))
        return target;
    target = lookup_function_value_alias_target(ctx, sym);
    return target ? target : sym;
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

static bool es_walk_immediate_function_expr_call(ErrorSetCtx *ctx, AstNode *callee) {
    AstNode *source = identity_source(callee);
    if (!ctx || !source || source->type != AST_FUNCTION_EXPR)
        return false;
    FunctionDeclNode *fn = &source->as.function_expr;
    if (!fn->body)
        return true;

    XaScope *saved_scope = ctx->analyzer->current_scope;
    XaScope *fn_scope = xa_scope_find_by_node(ctx->analyzer->global_scope, source);
    if (fn_scope)
        ctx->analyzer->current_scope = fn_scope;
    es_walk_block(ctx, fn->body);
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
        if (ctx->current_catch_symbol_id != 0 && symbol_id == ctx->current_catch_symbol_id)
            return true;
        for (int i = 0; i < ctx->current_catch_alias_count; i++) {
            if (ctx->current_catch_alias_ids[i] != 0 &&
                symbol_id == ctx->current_catch_alias_ids[i])
                return true;
        }
        return ctx->current_catch_var &&
               strcmp(expr->as.variable.name, ctx->current_catch_var) == 0;
    }

    if (ctx->current_catch_var && strcmp(expr->as.variable.name, ctx->current_catch_var) == 0)
        return true;
    for (int i = 0; i < ctx->current_catch_alias_count; i++) {
        const char *name = ctx->current_catch_alias_names[i];
        if (name && strcmp(expr->as.variable.name, name) == 0)
            return true;
    }
    return false;
}

static void maybe_record_catch_alias(ErrorSetCtx *ctx, AstNode *node) {
    if (!ctx || !node || !ctx->current_caught || node->type != AST_CONST_DECL)
        return;
    VarDeclNode *decl = &node->as.var_decl;
    if (!decl->name || !decl->initializer || decl->symbol_id == 0 ||
        ctx->current_catch_alias_count >= 64)
        return;
    if (!is_current_caught_ref(ctx, decl->initializer))
        return;
    int slot = ctx->current_catch_alias_count++;
    ctx->current_catch_alias_ids[slot] = decl->symbol_id;
    ctx->current_catch_alias_names[slot] = decl->name;
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
            XaSymbol *callee_sym = resolve_call_target(ctx, node->as.call_expr.callee);
            if (callee_sym &&
                (callee_sym->kind == XA_SYM_FUNCTION || callee_sym->kind == XA_SYM_METHOD) &&
                callee_sym->links.effect_id != XA_EFFECT_NONE) {
                const XaEffectSummary *callee_summary =
                    xa_effect_db_get(ctx->analyzer->effect_db, callee_sym->links.effect_id);
                if (callee_summary)
                    xa_effect_summary_add_summary(ctx->analyzer->effect_db, ctx->current_summary,
                                                  callee_summary);
            } else if (is_dynamic_function_call_target(ctx->analyzer, node->as.call_expr.callee,
                                                       callee_sym)) {
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
        ctx->current_catch_alias_count = saved_catch_alias_count;
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

            if (is_current_caught_ref(ctx, expr)) {
                xa_effect_summary_add_summary(ctx->analyzer->effect_db, ctx->current_summary,
                                              ctx->current_caught);
                break;
            }

            const XaSelection *throw_sel = xa_analyzer_get_selection(ctx->analyzer, expr);
            if (throw_sel && throw_sel->kind == XA_SEL_ENUM_MEMBER) {
                XrType *enum_type = throw_sel->result_type;
                if (!enum_type && throw_sel->target_symbol)
                    enum_type = throw_sel->target_symbol->links.type;
                bool handled_selection = false;
                if (enum_type && throw_sel->field_index >= 0) {
                    es_summary_add_enum_case(ctx->analyzer->effect_db, ctx->current_summary,
                                             enum_type, (uint32_t) throw_sel->field_index);
                    handled_selection = true;
                } else if (enum_type) {
                    es_summary_add_enum_all(ctx->analyzer->effect_db, ctx->current_summary,
                                            enum_type);
                    handled_selection = true;
                }
                if (handled_selection)
                    break;
            }

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
            ctx->current_summary = &try_summary;
            es_walk_block(ctx, tc->try_body);
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
                        int saved_catch_alias_count = ctx->current_catch_alias_count;
                        XaEffectSummary *saved_caught = ctx->current_caught;
                        ctx->current_catch_var = cc->var_name;
                        ctx->current_catch_symbol_id = cc->symbol_id;
                        ctx->current_catch_alias_count = 0;
                        ctx->current_caught = caught_summaries ? &caught_summaries[i] : NULL;
                        es_walk_block(ctx, cc->body);
                        ctx->current_catch_var = saved_catch_var;
                        ctx->current_catch_symbol_id = saved_catch_symbol_id;
                        ctx->current_catch_alias_count = saved_catch_alias_count;
                        ctx->current_caught = saved_caught;
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
            xa_effect_summary_clear(&try_summary);
            break;
        }

        case AST_EXPR_STMT:
            es_walk_expr(ctx, node->as.expr_stmt);
            break;

        case AST_VAR_DECL:
            maybe_record_stable_function_value_var(ctx, node);
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
            invalidate_function_value_alias_target(ctx, node->as.assignment.symbol_id);
            es_walk_expr(ctx, node->as.assignment.value);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++)
                es_walk_expr(ctx, node->as.return_stmt.values[i]);
            break;

        case AST_IF_STMT:
            es_walk_expr(ctx, node->as.if_stmt.condition);
            es_walk_block(ctx, node->as.if_stmt.then_branch);
            es_walk_block(ctx, node->as.if_stmt.else_branch);
            break;

        case AST_WHILE_STMT:
            es_walk_expr(ctx, node->as.while_stmt.condition);
            es_walk_block(ctx, node->as.while_stmt.body);
            break;

        case AST_FOR_STMT:
            es_walk_stmt(ctx, node->as.for_stmt.initializer);
            es_walk_expr(ctx, node->as.for_stmt.condition);
            es_walk_expr(ctx, node->as.for_stmt.increment);
            es_walk_block(ctx, node->as.for_stmt.body);
            break;

        case AST_FOR_IN_STMT:
            es_walk_expr(ctx, node->as.for_in_stmt.collection);
            es_walk_block(ctx, node->as.for_in_stmt.body);
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

    AstNode *body = func_node->as.function_decl.body;
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
    es_walk_block(ctx, body);
    XaEffectId previous_id = func_sym->links.effect_id;
    func_sym->links.effect_id = xa_effect_db_intern(ctx->analyzer->effect_db, &summary);
    if (func_sym->links.effect_id != previous_id)
        ctx->changed = true;
    xa_effect_summary_clear(&summary);

    ctx->current_summary = NULL;
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

    if (node->type == AST_FUNCTION_DECL) {
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
}
