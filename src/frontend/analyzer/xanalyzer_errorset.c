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
 * throw.  The result is stored in XaSymbolLinks.error_set and
 * propagated to XrType.function.error_set.
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
#include "../../runtime/value/xerror_set.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../base/xmalloc.h"
#include <string.h>
#include <stdio.h>

/* ========== Internal Context ========== */

typedef struct ErrorSetCtx {
    XaAnalyzer *analyzer;
    XrTypePool *pool;
    XrErrorSet *current_set; /* Error set being built for current function */
    XaSymbol *current_func;  /* Current function symbol */
    bool changed;            /* Fixpoint: did anything change this iteration? */
} ErrorSetCtx;

/* ========== Forward Declarations ========== */

static void es_walk_stmt(ErrorSetCtx *ctx, AstNode *node);
static void es_walk_expr(ErrorSetCtx *ctx, AstNode *node);

/* ========== Helpers ========== */

static XaSymbol *resolve_func_symbol(XaAnalyzer *analyzer, AstNode *node) {
    if (!node)
        return NULL;
    if (node->type == AST_FUNCTION_DECL) {
        const char *name = node->as.function_decl.name;
        if (name)
            return xa_scope_lookup(analyzer->global_scope, name);
    }
    return NULL;
}

/* Resolve a call target to its function symbol.
 * Handles direct calls (foo()) and simple member calls. */
static XaSymbol *resolve_call_target(XaAnalyzer *analyzer, AstNode *callee) {
    if (!callee)
        return NULL;
    if (callee->type == AST_VARIABLE) {
        return xa_scope_lookup(analyzer->global_scope, callee->as.variable.name);
    }
    return NULL;
}

/* Try to find the case index of an enum member.
 * Searches the enum symbol's member list. */
static int find_enum_case_index(XaSymbol *enum_sym, const char *case_name) {
    if (!enum_sym || !case_name)
        return -1;
    XaSymbolLinks *links = &enum_sym->links;
    for (int i = 0; i < links->enum_member_count; i++) {
        if (links->enum_member_names[i] && strcmp(links->enum_member_names[i], case_name) == 0) {
            return i;
        }
    }
    return -1;
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

            /* Union callee's error set into current function's error set */
            XaSymbol *callee_sym = resolve_call_target(ctx->analyzer, node->as.call_expr.callee);
            if (callee_sym && callee_sym->links.error_set) {
                XrErrorSet *old_snapshot = ctx->current_set;
                int old_count = old_snapshot ? old_snapshot->count : 0;
                xr_error_set_union(ctx->pool, ctx->current_set, callee_sym->links.error_set);
                if (ctx->current_set->count != old_count)
                    ctx->changed = true;
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
            for (int i = 0; i < node->as.array_literal.count; i++)
                es_walk_expr(ctx, node->as.array_literal.elements[i]);
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
        for (int i = 0; i < node->as.block.count; i++)
            es_walk_stmt(ctx, node->as.block.statements[i]);
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

            /*
             * Handle `throw EnumName.CaseName` — add the specific case.
             * Handle `throw variable` where variable has enum type — add all cases.
             */
            if (expr->type == AST_ENUM_ACCESS) {
                const char *enum_name = expr->as.enum_access.enum_name;
                const char *member_name = expr->as.enum_access.member_name;
                if (enum_name) {
                    XaSymbol *enum_sym = xa_scope_lookup(ctx->analyzer->global_scope, enum_name);
                    if (enum_sym && enum_sym->kind == XA_SYM_ENUM) {
                        XrType *enum_type = enum_sym->links.type;
                        int case_idx = find_enum_case_index(enum_sym, member_name);
                        if (enum_type && case_idx >= 0) {
                            int old_count = ctx->current_set->count;
                            xr_error_set_add_case(ctx->pool, ctx->current_set, enum_type, case_idx);
                            if (ctx->current_set->count != old_count)
                                ctx->changed = true;
                        } else if (enum_type) {
                            int old_count = ctx->current_set->count;
                            xr_error_set_add_all(ctx->pool, ctx->current_set, enum_type);
                            if (ctx->current_set->count != old_count)
                                ctx->changed = true;
                        }
                    }
                }
            } else {
                /* Generic throw: infer type from the expression's analyzed type */
                XrType *thrown_type = xa_analyzer_get_node_type(ctx->analyzer, expr);
                if (thrown_type && XR_TYPE_IS_ENUM(thrown_type)) {
                    int old_count = ctx->current_set->count;
                    xr_error_set_add_all(ctx->pool, ctx->current_set, thrown_type);
                    if (ctx->current_set->count != old_count)
                        ctx->changed = true;
                }
            }
            break;
        }

        case AST_TRY_CATCH: {
            TryCatchNode *tc = &node->as.try_catch;

            /* Walk try body — errors from here are candidates for catch */
            es_walk_block(ctx, tc->try_body);

            /* Catch-all removes all errors; typed/pattern catch support must
             * filter only matching error variants. */
            if (tc->catch_count > 0) {
                for (int i = 0; i < tc->catch_count; i++) {
                    XrCatchClause *cc = tc->catch_clauses[i];
                    if (cc && cc->body) {
                        es_walk_block(ctx, cc->body);
                    }
                }
            }
            break;
        }

        case AST_EXPR_STMT:
            es_walk_expr(ctx, node->as.expr_stmt);
            break;

        case AST_VAR_DECL:
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_CONST_DECL:
            es_walk_expr(ctx, node->as.var_decl.initializer);
            break;

        case AST_ASSIGNMENT:
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

    /* Create or reuse existing error set */
    if (!func_sym->links.error_set)
        func_sym->links.error_set = xr_error_set_new(ctx->pool);

    ctx->current_set = func_sym->links.error_set;
    es_walk_block(ctx, body);

    /* Propagate to function XrType */
    XrType *ftype = func_sym->links.type;
    if (ftype && ftype->kind == XR_KIND_FUNCTION) {
        if (xr_error_set_is_empty(func_sym->links.error_set)) {
            ftype->function.error_set = NULL;
        } else {
            ftype->function.error_set = func_sym->links.error_set;
        }
    }

    ctx->current_func = NULL;
    ctx->current_set = NULL;
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
    ctx.pool = analyzer->type_pool;

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
