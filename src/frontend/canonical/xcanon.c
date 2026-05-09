/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcanon.c - Typed AST canonicalizer implementation
 *
 * KEY CONCEPT:
 *   Transforms post-analysis AST into canonical form for the lowerer.
 *   Canonicalization rules:
 *     - Compound assignment desugaring (variable and member targets)
 *     - Increment/decrement expansion (x++ → x += 1 → x = x + 1)
 *     - Index-set receiver extraction (statement context only)
 *     - Short-circuit logic expansion (&& / || → ternary)
 *     - Nullish coalesce expansion (?? → null-check ternary)
 */

#include "xcanon.h"
#include "../../base/xchecks.h"
#include "../../base/xdefs.h"
#include "../parser/xast_nodes.h"
#include "../parser/xast_api.h"
#include "../parser/xparse_internal.h"
#include "../analyzer/xanalyzer.h"

#include <stdio.h>
#include <string.h>

/* ========== Canonicalizer context ========== */

typedef struct XrCanonCtx {
    struct XaAnalyzer *analyzer;
    struct XrayIsolate *isolate;
    uint32_t temp_counter; /* monotonic counter for generating unique temp names */
    int error_count;
} XrCanonCtx;

/* ========== Temp variable helpers ========== */

/* Maximum temp name length: "__canon_" (8) + digits (10) + NUL = 19 */
#define CANON_TEMP_PREFIX "__canon_"
#define CANON_TEMP_BUFSZ 24

/* Generate a unique temp variable name, arena-allocated. */
static char *canon_temp_name(XrCanonCtx *ctx) {
    char buf[CANON_TEMP_BUFSZ];
    int n = snprintf(buf, sizeof(buf), CANON_TEMP_PREFIX "%u", ctx->temp_counter++);
    XR_DCHECK(n > 0 && n < (int) sizeof(buf), "canon_temp_name: snprintf overflow");
    return ast_strdup(ctx->isolate, buf);
}

/* ========== Expression classification ========== */

/* Returns true if expr is side-effect-free and cheap to evaluate twice.
 * Used to decide whether a receiver needs extraction into a temp. */
static bool is_simple_expr(const AstNode *node) {
    if (!node)
        return true;
    switch (node->type) {
        case AST_VARIABLE:
        case AST_THIS_EXPR:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
            return true;
        default:
            return false;
    }
}

/* ========== Operator mapping ========== */

/* Map compound-assignment token to the corresponding binary AST type. */
static AstNodeType compound_op_to_binary(XrTokenType op) {
    switch (op) {
        case TK_PLUS_ASSIGN:
            return AST_BINARY_ADD;
        case TK_MINUS_ASSIGN:
            return AST_BINARY_SUB;
        case TK_MUL_ASSIGN:
            return AST_BINARY_MUL;
        case TK_DIV_ASSIGN:
            return AST_BINARY_DIV;
        case TK_MOD_ASSIGN:
            return AST_BINARY_MOD;
        case TK_AND_ASSIGN:
            return AST_BINARY_BAND;
        case TK_OR_ASSIGN:
            return AST_BINARY_BOR;
        case TK_XOR_ASSIGN:
            return AST_BINARY_BXOR;
        case TK_LSHIFT_ASSIGN:
            return AST_BINARY_LSHIFT;
        case TK_RSHIFT_ASSIGN:
            return AST_BINARY_RSHIFT;
        default:
            return AST_BINARY_ADD;
    }
}

/* ========== Compound assignment desugaring ========== */

/* Desugar simple variable compound assignment in-place.
 *   x += e  →  x = x + e
 *
 * The node is mutated: type changes to AST_ASSIGNMENT, the
 * union payload switches from CompoundAssignmentNode to
 * AssignmentNode.  The symbol_id is preserved so the lowerer
 * resolves the target identically. */
static void canon_compound_var(XrCanonCtx *ctx, AstNode *node) {
    XR_DCHECK(node->type == AST_COMPOUND_ASSIGNMENT, "canon_compound_var: wrong node type");
    XR_DCHECK(node->as.compound_assignment.object == NULL,
              "canon_compound_var: member target not expected");

    const char *name = node->as.compound_assignment.name;
    uint32_t sid = node->as.compound_assignment.symbol_id;
    XrTokenType op = node->as.compound_assignment.op;
    AstNode *rhs = node->as.compound_assignment.value;
    int line = node->line;

    /* Build: variable(name) */
    AstNode *lhs_read = xr_ast_variable(ctx->isolate, name, line);
    XR_DCHECK(lhs_read != NULL, "canon_compound_var: failed to create variable ref");
    lhs_read->as.variable.symbol_id = sid;

    /* Build: variable(name) op rhs */
    AstNodeType bin_type = compound_op_to_binary(op);
    AstNode *bin = xr_ast_binary(ctx->isolate, bin_type, lhs_read, rhs, line);
    XR_DCHECK(bin != NULL, "canon_compound_var: failed to create binary");

    /* Mutate in-place: AST_COMPOUND_ASSIGNMENT → AST_ASSIGNMENT */
    node->type = AST_ASSIGNMENT;
    memset(&node->as, 0, sizeof(node->as));
    node->as.assignment.name = (char *) name;
    node->as.assignment.value = bin;
    node->as.assignment.symbol_id = sid;
}

/* Desugar member compound assignment.
 *   obj.field += e  →  obj.field = obj.field + e    (simple receiver)
 *   f().field += e  →  { let __t = f(); __t.field = __t.field + e }
 *
 * When the receiver is complex (call, binary, etc.) it is extracted
 * into a temp variable to ensure single evaluation and correct
 * left-to-right order.  The node is mutated in-place: for the simple
 * case it becomes AST_MEMBER_SET; for the complex case it becomes
 * AST_BLOCK wrapping a var_decl + member_set. */
static void canon_compound_member(XrCanonCtx *ctx, AstNode *node) {
    XR_DCHECK(node->type == AST_COMPOUND_ASSIGNMENT, "canon_compound_member: wrong node type");
    XR_DCHECK(node->as.compound_assignment.object != NULL,
              "canon_compound_member: no receiver object");

    AstNode *obj = node->as.compound_assignment.object;
    const char *field = node->as.compound_assignment.name;
    XrTokenType op = node->as.compound_assignment.op;
    AstNode *rhs = node->as.compound_assignment.value;
    int line = node->line;
    AstNodeType bin_type = compound_op_to_binary(op);

    if (is_simple_expr(obj)) {
        /* obj is cheap — safe to reference it twice.
         * Build: obj.field = obj.field + rhs */
        AstNode *load = xr_ast_member_access(ctx->isolate, obj, field, line);
        XR_DCHECK(load != NULL, "canon_compound_member: member_access alloc");
        AstNode *bin = xr_ast_binary(ctx->isolate, bin_type, load, rhs, line);
        XR_DCHECK(bin != NULL, "canon_compound_member: binary alloc");

        /* Mutate in-place → AST_MEMBER_SET */
        node->type = AST_MEMBER_SET;
        memset(&node->as, 0, sizeof(node->as));
        node->as.member_set.object = obj;
        node->as.member_set.member = (char *) field;
        node->as.member_set.value = bin;
    } else {
        /* Complex receiver — extract into a temp. */
        char *tmp = canon_temp_name(ctx);
        XR_DCHECK(tmp != NULL, "canon_compound_member: temp name alloc");

        /* let __canon_N = obj */
        AstNode *decl = xr_ast_var_decl(ctx->isolate, tmp, obj, false, line);
        XR_DCHECK(decl != NULL, "canon_compound_member: var_decl alloc");

        /* __canon_N.field = __canon_N.field + rhs */
        AstNode *ref1 = xr_ast_variable(ctx->isolate, tmp, line);
        AstNode *ref2 = xr_ast_variable(ctx->isolate, tmp, line);
        AstNode *load = xr_ast_member_access(ctx->isolate, ref1, field, line);
        AstNode *bin = xr_ast_binary(ctx->isolate, bin_type, load, rhs, line);
        AstNode *store = xr_ast_member_set(ctx->isolate, ref2, field, bin, line);
        XR_DCHECK(store != NULL, "canon_compound_member: member_set alloc");

        /* Wrap in block: { decl; store } */
        AstNode *blk = xr_ast_block(ctx->isolate, line);
        XR_DCHECK(blk != NULL, "canon_compound_member: block alloc");
        xr_ast_block_add(ctx->isolate, blk, decl);
        xr_ast_block_add(ctx->isolate, blk, store);

        /* Mutate the original node in-place → AST_BLOCK */
        uint32_t saved_id = node->node_id;
        *node = *blk;
        node->node_id = saved_id;
    }
}

/* Top-level dispatch for compound assignment desugaring. */
static void canon_compound_assignment(XrCanonCtx *ctx, AstNode *node) {
    if (!node || node->type != AST_COMPOUND_ASSIGNMENT)
        return;

    if (node->as.compound_assignment.object) {
        canon_compound_member(ctx, node);
    } else {
        canon_compound_var(ctx, node);
    }
}

/* ========== Increment / Decrement desugaring ========== */

/* Desugar x++ / x-- into compound assignment: x += 1 / x -= 1.
 * The compound assignment rule then further expands to x = x + 1.
 * This ensures inc/dec follows the same code path as compound assign. */
static void canon_inc_dec(XrCanonCtx *ctx, AstNode *node) {
    XR_DCHECK(node->type == AST_INC || node->type == AST_DEC, "canon_inc_dec: wrong node type");

    const char *name = node->as.inc.name;
    uint32_t sid = node->as.inc.symbol_id;
    int line = node->line;
    bool is_inc = (node->type == AST_INC);

    /* Build literal 1 */
    AstNode *one = xr_ast_literal_int(ctx->isolate, 1, line);
    XR_DCHECK(one != NULL, "canon_inc_dec: literal alloc");

    /* Mutate in-place → AST_COMPOUND_ASSIGNMENT(name, +=/-=, 1) */
    node->type = AST_COMPOUND_ASSIGNMENT;
    memset(&node->as, 0, sizeof(node->as));
    node->as.compound_assignment.name = (char *) name;
    node->as.compound_assignment.value = one;
    node->as.compound_assignment.symbol_id = sid;
    node->as.compound_assignment.op = is_inc ? TK_PLUS_ASSIGN : TK_MINUS_ASSIGN;
    node->as.compound_assignment.object = NULL;

    /* Immediately desugar the synthesized compound assignment */
    canon_compound_assignment(ctx, node);
}

/* ========== Index-set receiver extraction ========== */

/* Desugar index-set with complex array or index expressions.
 *   f()[g()] = v  →  { let __t0 = f(); let __t1 = g(); __t0[__t1] = v }
 * This ensures correct left-to-right single evaluation of sub-expressions. */
static void canon_index_set(XrCanonCtx *ctx, AstNode *node) {
    XR_DCHECK(node->type == AST_INDEX_SET, "canon_index_set: wrong node type");

    AstNode *arr = node->as.index_set.array;
    AstNode *idx = node->as.index_set.index;
    bool arr_complex = !is_simple_expr(arr);
    bool idx_complex = !is_simple_expr(idx);

    /* Both sides simple — no extraction needed */
    if (!arr_complex && !idx_complex)
        return;

    int line = node->line;
    AstNode *blk = xr_ast_block(ctx->isolate, line);
    XR_DCHECK(blk != NULL, "canon_index_set: block alloc");

    AstNode *new_arr = arr;
    AstNode *new_idx = idx;

    if (arr_complex) {
        char *tmp_arr = canon_temp_name(ctx);
        AstNode *decl_arr = xr_ast_var_decl(ctx->isolate, tmp_arr, arr, false, line);
        XR_DCHECK(decl_arr != NULL, "canon_index_set: var_decl alloc");
        xr_ast_block_add(ctx->isolate, blk, decl_arr);
        new_arr = xr_ast_variable(ctx->isolate, tmp_arr, line);
    }

    if (idx_complex) {
        char *tmp_idx = canon_temp_name(ctx);
        AstNode *decl_idx = xr_ast_var_decl(ctx->isolate, tmp_idx, idx, false, line);
        XR_DCHECK(decl_idx != NULL, "canon_index_set: var_decl alloc");
        xr_ast_block_add(ctx->isolate, blk, decl_idx);
        new_idx = xr_ast_variable(ctx->isolate, tmp_idx, line);
    }

    /* Build simplified index_set with simple operands */
    AstNode *store =
        xr_ast_index_set(ctx->isolate, new_arr, new_idx, node->as.index_set.value, line);
    XR_DCHECK(store != NULL, "canon_index_set: index_set alloc");
    xr_ast_block_add(ctx->isolate, blk, store);

    /* Mutate in-place → AST_BLOCK */
    uint32_t saved_id = node->node_id;
    *node = *blk;
    node->node_id = saved_id;
}

/* ========== Short-circuit logic expansion ========== */

/* Build !!expr (double-not) to coerce any value to bool.
 * If the expr is already a bool literal, returns it directly. */
static AstNode *make_bool_coerce(XrCanonCtx *ctx, AstNode *expr) {
    XR_DCHECK(expr != NULL, "make_bool_coerce: NULL expr");
    /* Skip coercion for bool literals */
    if (expr->type == AST_LITERAL_TRUE || expr->type == AST_LITERAL_FALSE)
        return expr;
    int line = expr->line;
    AstNode *not1 = xr_ast_unary(ctx->isolate, AST_UNARY_NOT, expr, line);
    XR_DCHECK(not1 != NULL, "make_bool_coerce: not1 alloc");
    AstNode *not2 = xr_ast_unary(ctx->isolate, AST_UNARY_NOT, not1, line);
    XR_DCHECK(not2 != NULL, "make_bool_coerce: not2 alloc");
    return not2;
}

/* Expand short-circuit logic into explicit ternary (control flow).
 *   a && b  →  a ? !!b : false
 *   a || b  →  a ? true : !!b
 *
 * This makes control flow explicit at the AST level, so the lowerer
 * handles it as a standard ternary (no special short-circuit path). */
static void canon_short_circuit(XrCanonCtx *ctx, AstNode *node) {
    XR_DCHECK(node->type == AST_BINARY_AND || node->type == AST_BINARY_OR,
              "canon_short_circuit: wrong node type");

    bool is_and = (node->type == AST_BINARY_AND);
    AstNode *lhs = node->as.binary.left;
    AstNode *rhs = node->as.binary.right;
    int line = node->line;

    AstNode *true_expr;
    AstNode *false_expr;

    if (is_and) {
        /* a && b → a ? !!b : false */
        true_expr = make_bool_coerce(ctx, rhs);
        false_expr = xr_ast_literal_bool(ctx->isolate, 0, line);
    } else {
        /* a || b → a ? true : !!b */
        true_expr = xr_ast_literal_bool(ctx->isolate, 1, line);
        false_expr = make_bool_coerce(ctx, rhs);
    }
    XR_DCHECK(true_expr != NULL && false_expr != NULL, "canon_short_circuit: branch alloc");

    AstNode *ternary = xr_ast_ternary(ctx->isolate, lhs, true_expr, false_expr, line);
    XR_DCHECK(ternary != NULL, "canon_short_circuit: ternary alloc");

    /* Mutate in-place → AST_TERNARY */
    uint32_t saved_id = node->node_id;
    *node = *ternary;
    node->node_id = saved_id;
}

/* ========== Nullish coalesce expansion ========== */

/* Expand ?? into explicit null-check ternary.
 *   x ?? b          →  x == null ? b : x          (x is simple)
 *   f() ?? b        →  (let __t = f(); __t == null ? b : __t)
 *
 * For simple LHS we emit the ternary directly (no temp needed since
 * variables/literals have no side effects and evaluate identically).
 * Complex LHS requires statement context for temp extraction, so
 * canon_block handles that path. In expression context we only
 * handle simple LHS; complex LHS falls through to the lowerer. */
static void canon_nullish_coalesce(XrCanonCtx *ctx, AstNode *node, bool in_stmt_context) {
    XR_DCHECK(node->type == AST_NULLISH_COALESCE, "canon_nullish_coalesce: wrong node type");

    AstNode *lhs = node->as.binary.left;
    AstNode *rhs = node->as.binary.right;
    int line = node->line;
    bool lhs_simple = is_simple_expr(lhs);

    /* Complex LHS in expression context — cannot safely extract temp */
    if (!lhs_simple && !in_stmt_context)
        return;

    AstNode *test_lhs = lhs; /* the value to null-check and return */

    if (!lhs_simple) {
        /* Statement context: wrap in a block with temp extraction.
         * Not applicable here since canon_block calls us before
         * canon_node; but guard for future callers. */
        return;
    }

    /* Build: lhs == null ? rhs : lhs */
    AstNode *null_lit = xr_ast_literal_null(ctx->isolate, line);
    XR_DCHECK(null_lit != NULL, "canon_nullish_coalesce: null alloc");

    AstNode *eq_check = xr_ast_binary(ctx->isolate, AST_BINARY_EQ, test_lhs, null_lit, line);
    XR_DCHECK(eq_check != NULL, "canon_nullish_coalesce: eq alloc");

    /* Duplicate the LHS reference for the false branch.
     * Since lhs is simple (variable/literal), safe to reuse pointer. */
    AstNode *ternary = xr_ast_ternary(ctx->isolate, eq_check, rhs, test_lhs, line);
    XR_DCHECK(ternary != NULL, "canon_nullish_coalesce: ternary alloc");

    uint32_t saved_id = node->node_id;
    *node = *ternary;
    node->node_id = saved_id;
}

/* ========== AST walk (recursive, dispatches on node type) ========== */

/* Forward declaration — the walker calls itself recursively. */
static void canon_node(XrCanonCtx *ctx, AstNode *node);

/* Canonicalize a block's statements in order.
 * Statements in block context can safely be replaced by AST_BLOCK
 * (e.g. index_set with complex receivers). */
static void canon_block(XrCanonCtx *ctx, AstNode **stmts, int count) {
    XR_DCHECK(ctx != NULL, "canon_block: NULL ctx");
    for (int i = 0; i < count; i++) {
        if (!stmts[i])
            continue;
        /* Statement-context canonicalizations that expand to blocks */
        if (stmts[i]->type == AST_INDEX_SET) {
            canon_index_set(ctx, stmts[i]);
        }
        canon_node(ctx, stmts[i]);
    }
}

/* Recursive canonicalization dispatcher.
 * Applies canonicalization rules first, then walks child nodes.
 * Rules may mutate the node type, so the switch runs on the
 * post-canonicalization type. */
static void canon_node(XrCanonCtx *ctx, AstNode *node) {
    if (!node)
        return;

    /* ---- Canonicalization rules (fire before recursive walk) ---- */
    if (node->type == AST_COMPOUND_ASSIGNMENT) {
        canon_compound_assignment(ctx, node);
        /* Node type has changed; fall through to walk the result. */
    }
    if (node->type == AST_INC || node->type == AST_DEC) {
        canon_inc_dec(ctx, node);
        /* Node type has changed to AST_ASSIGNMENT; fall through. */
    }
    /* AST_INDEX_SET canonicalization is applied in canon_block() (statement
     * context only) — expanding to AST_BLOCK in expression position would
     * break the lowerer. */
    if (node->type == AST_BINARY_AND || node->type == AST_BINARY_OR) {
        canon_short_circuit(ctx, node);
        /* Node type has changed to AST_TERNARY; fall through. */
    }
    if (node->type == AST_NULLISH_COALESCE) {
        canon_nullish_coalesce(ctx, node, /*in_stmt_context=*/false);
        /* May have changed to AST_TERNARY (simple LHS) or stayed as-is. */
    }

    switch (node->type) {
        /* ---- Statements with bodies ---- */
        case AST_BLOCK:
            canon_block(ctx, node->as.block.statements, node->as.block.count);
            break;

        case AST_PROGRAM:
            canon_block(ctx, node->as.program.statements, node->as.program.count);
            break;

        case AST_IF_STMT:
            canon_node(ctx, node->as.if_stmt.condition);
            canon_node(ctx, node->as.if_stmt.then_branch);
            canon_node(ctx, node->as.if_stmt.else_branch);
            break;

        case AST_WHILE_STMT:
            canon_node(ctx, node->as.while_stmt.condition);
            canon_node(ctx, node->as.while_stmt.body);
            break;

        case AST_FOR_STMT:
            canon_node(ctx, node->as.for_stmt.initializer);
            canon_node(ctx, node->as.for_stmt.condition);
            canon_node(ctx, node->as.for_stmt.increment);
            canon_node(ctx, node->as.for_stmt.body);
            break;

        case AST_FOR_IN_STMT:
            canon_node(ctx, node->as.for_in_stmt.collection);
            canon_node(ctx, node->as.for_in_stmt.body);
            break;

        case AST_FUNCTION_DECL:
        case AST_FUNCTION_EXPR:
            canon_node(ctx, node->as.function_decl.body);
            break;

        case AST_TRY_CATCH:
            canon_node(ctx, node->as.try_catch.try_body);
            canon_node(ctx, node->as.try_catch.catch_body);
            canon_node(ctx, node->as.try_catch.finally_body);
            break;

        case AST_MATCH_EXPR: {
            MatchExprNode *m = &node->as.match_expr;
            canon_node(ctx, m->expr);
            for (int i = 0; i < m->arm_count; i++) {
                if (m->arms[i])
                    canon_node(ctx, m->arms[i]);
            }
            break;
        }

        /* AST_MATCH_ARM handled below (walks guard + body) */

        /* ---- Expressions (walk children) ---- */
        case AST_EXPR_STMT:
            canon_node(ctx, node->as.expr_stmt);
            break;

        case AST_VAR_DECL:
        case AST_CONST_DECL:
            canon_node(ctx, node->as.var_decl.initializer);
            break;

        case AST_ASSIGNMENT:
            canon_node(ctx, node->as.assignment.value);
            break;

        case AST_RETURN_STMT:
            for (int i = 0; i < node->as.return_stmt.value_count; i++) {
                canon_node(ctx, node->as.return_stmt.values[i]);
            }
            break;

        case AST_CALL_EXPR: {
            CallExprNode *call = &node->as.call_expr;
            canon_node(ctx, call->callee);
            for (int i = 0; i < call->arg_count; i++) {
                if (call->arguments[i])
                    canon_node(ctx, call->arguments[i]);
            }
            break;
        }

        case AST_MEMBER_ACCESS:
            canon_node(ctx, node->as.member_access.object);
            break;

        case AST_MEMBER_SET:
            canon_node(ctx, node->as.member_set.object);
            canon_node(ctx, node->as.member_set.value);
            break;

        case AST_INDEX_GET:
            canon_node(ctx, node->as.index_get.array);
            canon_node(ctx, node->as.index_get.index);
            break;

        case AST_INDEX_SET:
            canon_node(ctx, node->as.index_set.array);
            canon_node(ctx, node->as.index_set.index);
            canon_node(ctx, node->as.index_set.value);
            break;

        case AST_TERNARY:
            canon_node(ctx, node->as.ternary.condition);
            canon_node(ctx, node->as.ternary.true_expr);
            canon_node(ctx, node->as.ternary.false_expr);
            break;

        case AST_SCOPE_BLOCK:
            canon_node(ctx, node->as.scope_block.body);
            break;

        case AST_DEFER_STMT:
            canon_node(ctx, node->as.defer_stmt.expr);
            break;

        /* ---- Print statement ---- */
        case AST_PRINT_STMT: {
            PrintNode *p = &node->as.print_stmt;
            for (int i = 0; i < p->expr_count; i++)
                canon_node(ctx, p->exprs[i]);
            break;
        }

        /* ---- Throw ---- */
        case AST_THROW_STMT:
            canon_node(ctx, node->as.throw_stmt.expression);
            break;

        /* ---- Aggregate literals ---- */
        case AST_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.count; i++)
                canon_node(ctx, node->as.array_literal.elements[i]);
            break;

        case AST_MAP_LITERAL:
            for (int i = 0; i < node->as.map_literal.count; i++) {
                canon_node(ctx, node->as.map_literal.keys[i]);
                canon_node(ctx, node->as.map_literal.values[i]);
            }
            break;

        case AST_SET_LITERAL:
            for (int i = 0; i < node->as.set_literal.count; i++)
                canon_node(ctx, node->as.set_literal.elements[i]);
            break;

        case AST_OBJECT_LITERAL:
            for (int i = 0; i < node->as.object_literal.count; i++) {
                canon_node(ctx, node->as.object_literal.keys[i]);
                canon_node(ctx, node->as.object_literal.values[i]);
            }
            break;

        case AST_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.field_count; i++)
                canon_node(ctx, node->as.struct_literal.field_values[i]);
            break;

        case AST_TEMPLATE_STRING:
            for (int i = 0; i < node->as.template_str.part_count; i++)
                canon_node(ctx, node->as.template_str.parts[i]);
            break;

        /* ---- Slice ---- */
        case AST_SLICE_EXPR:
            canon_node(ctx, node->as.slice_expr.source);
            canon_node(ctx, node->as.slice_expr.start);
            canon_node(ctx, node->as.slice_expr.end);
            break;

        /* ---- Optional chain ---- */
        case AST_OPTIONAL_CHAIN:
            canon_node(ctx, node->as.optional_chain.object);
            if (node->as.optional_chain.index)
                canon_node(ctx, node->as.optional_chain.index);
            break;

        /* ---- Grouping / force unwrap (unary layout) ---- */
        case AST_GROUPING:
            canon_node(ctx, node->as.grouping);
            break;

        case AST_FORCE_UNWRAP:
            canon_node(ctx, node->as.unary.operand);
            break;

        /* ---- Type-checking expressions (walk the operand) ---- */
        case AST_IS_EXPR:
            canon_node(ctx, node->as.is_expr.expr);
            break;

        case AST_AS_EXPR:
            canon_node(ctx, node->as.as_expr.expr);
            break;

        /* ---- OOP: new / super ---- */
        case AST_NEW_EXPR:
            for (int i = 0; i < node->as.new_expr.arg_count; i++)
                canon_node(ctx, node->as.new_expr.arguments[i]);
            break;

        case AST_SUPER_CALL:
            for (int i = 0; i < node->as.super_call.arg_count; i++)
                canon_node(ctx, node->as.super_call.arguments[i]);
            break;

        /* ---- Coroutine / concurrency ---- */
        case AST_GO_EXPR:
            canon_node(ctx, node->as.go_expr.expr);
            if (node->as.go_expr.priority)
                canon_node(ctx, node->as.go_expr.priority);
            break;

        case AST_AWAIT_EXPR:
            canon_node(ctx, node->as.await_expr.expr);
            if (node->as.await_expr.timeout)
                canon_node(ctx, node->as.await_expr.timeout);
            break;

        case AST_CHANNEL_NEW:
            canon_node(ctx, node->as.channel_new.buffer_size);
            break;

        case AST_MOVE_EXPR:
            canon_node(ctx, node->as.move_expr.expr);
            break;

        case AST_SELECT_STMT: {
            SelectStmtNode *sel = &node->as.select_stmt;
            for (int i = 0; i < sel->case_count; i++) {
                if (!sel->cases[i])
                    continue;
                SelectCaseNode *sc = &sel->cases[i]->as.select_case;
                canon_node(ctx, sc->channel);
                canon_node(ctx, sc->value);
                canon_node(ctx, sc->body);
            }
            break;
        }

        /* ---- Multi-value declarations / assignments ---- */
        case AST_MULTI_VAR_DECL:
            for (int i = 0; i < node->as.multi_var_decl.value_count; i++)
                canon_node(ctx, node->as.multi_var_decl.values[i]);
            break;

        case AST_MULTI_ASSIGN:
            for (int i = 0; i < node->as.multi_assign.target_count; i++)
                canon_node(ctx, node->as.multi_assign.targets[i]);
            for (int i = 0; i < node->as.multi_assign.value_count; i++)
                canon_node(ctx, node->as.multi_assign.values[i]);
            break;

        /* ---- Destructuring ---- */
        case AST_DESTRUCTURE_DECL:
            canon_node(ctx, node->as.destructure_decl.initializer);
            break;

        case AST_DESTRUCTURE_ASSIGN:
            canon_node(ctx, node->as.destructure_assign.value);
            break;

        /* ---- Export (walk inner declaration) ---- */
        case AST_EXPORT_STMT:
            canon_node(ctx, node->as.export_stmt.declaration);
            break;

        /* ---- Class / struct (walk fields and methods) ---- */
        case AST_CLASS_DECL:
        case AST_STRUCT_DECL: {
            ClassDeclNode *cls = &node->as.class_decl;
            for (int i = 0; i < cls->field_count; i++)
                canon_node(ctx, cls->fields[i]);
            for (int i = 0; i < cls->method_count; i++)
                canon_node(ctx, cls->methods[i]);
            break;
        }

        /* ---- Enum (walk member values) ---- */
        case AST_ENUM_DECL:
            for (int i = 0; i < node->as.enum_decl.member_count; i++)
                canon_node(ctx, node->as.enum_decl.members[i]);
            break;

        case AST_ENUM_CONVERT:
            canon_node(ctx, node->as.enum_convert.value_expr);
            break;

        case AST_ENUM_INDEX:
            canon_node(ctx, node->as.enum_index.collection);
            canon_node(ctx, node->as.enum_index.index_expr);
            break;

        /* ---- Match arm (walk guard + body) ---- */
        case AST_MATCH_ARM:
            canon_node(ctx, node->as.match_arm.guard);
            canon_node(ctx, node->as.match_arm.body);
            break;

        /* ---- Method declaration (class/struct method body + defaults) ---- */
        case AST_METHOD_DECL: {
            MethodDeclNode *m = &node->as.method_decl;
            canon_node(ctx, m->body);
            if (m->default_values) {
                for (int i = 0; i < m->param_count; i++)
                    canon_node(ctx, m->default_values[i]);
            }
            if (m->base_args) {
                for (int i = 0; i < m->base_arg_count; i++)
                    canon_node(ctx, m->base_args[i]);
            }
            break;
        }

        /* ---- Field initializer ---- */
        case AST_FIELD_DECL:
            canon_node(ctx, node->as.field_decl.initializer);
            break;

        /* ---- Binary / unary expressions ---- */
        case AST_BINARY_ADD:
        case AST_BINARY_SUB:
        case AST_BINARY_MUL:
        case AST_BINARY_DIV:
        case AST_BINARY_MOD:
        case AST_BINARY_EQ:
        case AST_BINARY_NE:
        case AST_BINARY_LT:
        case AST_BINARY_LE:
        case AST_BINARY_GT:
        case AST_BINARY_GE:
        case AST_BINARY_AND:
        case AST_BINARY_OR:
        case AST_BINARY_BAND:
        case AST_BINARY_BOR:
        case AST_BINARY_BXOR:
        case AST_BINARY_LSHIFT:
        case AST_BINARY_RSHIFT:
        case AST_BINARY_EQ_STRICT:
        case AST_BINARY_NE_STRICT:
        case AST_NULLISH_COALESCE:
        case AST_RANGE:
            canon_node(ctx, node->as.binary.left);
            canon_node(ctx, node->as.binary.right);
            break;

        case AST_UNARY_NEG:
        case AST_UNARY_NOT:
        case AST_UNARY_BNOT:
            canon_node(ctx, node->as.unary.operand);
            break;

        /* ---- Leaf nodes (no expression children) ---- */
        case AST_VARIABLE:
        case AST_LITERAL_INT:
        case AST_LITERAL_FLOAT:
        case AST_LITERAL_STRING:
        case AST_LITERAL_TRUE:
        case AST_LITERAL_FALSE:
        case AST_LITERAL_NULL:
        case AST_LITERAL_BIGINT:
        case AST_LITERAL_REGEX:
        case AST_BREAK_STMT:
        case AST_CONTINUE_STMT:
        case AST_THIS_EXPR:
        case AST_CANCELLED_EXPR:
        case AST_ENUM_ACCESS:
        case AST_ENUM_MEMBER:
        case AST_IMPORT_STMT:
        case AST_TYPE_ALIAS:
        case AST_INTERFACE_DECL:
        case AST_YIELD_STMT:
        case AST_PATTERN_LITERAL:
        case AST_PATTERN_RANGE:
        case AST_PATTERN_WILDCARD:
        case AST_PATTERN_MULTI:
            break;

        default:
            /* Unknown node type — should not reach here.
             * If a new AST node is added, extend this switch. */
            break;
    }
}

/* ========== Public API ========== */

XR_FUNC XrCanonStatus xr_canon_program(struct AstNode *program, struct XaAnalyzer *analyzer,
                                       struct XrayIsolate *isolate) {
    if (!program || !analyzer || !isolate)
        return XR_CANON_ERR_NULL_INPUT;

    XR_DCHECK(program->type == AST_PROGRAM || program->type == AST_BLOCK,
              "xr_canon_program: expected PROGRAM or BLOCK node");

    XrCanonCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.analyzer = analyzer;
    ctx.isolate = isolate;

    canon_node(&ctx, program);

    return ctx.error_count > 0 ? XR_CANON_ERR_INTERNAL : XR_CANON_OK;
}

XR_FUNC XrCanonStatus xr_canon_func(struct AstNode *func_node, struct XaAnalyzer *analyzer,
                                    struct XrayIsolate *isolate) {
    if (!func_node || !analyzer || !isolate)
        return XR_CANON_ERR_NULL_INPUT;

    XR_DCHECK(func_node->type == AST_FUNCTION_DECL || func_node->type == AST_FUNCTION_EXPR,
              "xr_canon_func: expected function node");

    XrCanonCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.analyzer = analyzer;
    ctx.isolate = isolate;

    canon_node(&ctx, func_node);

    return ctx.error_count > 0 ? XR_CANON_ERR_INTERNAL : XR_CANON_OK;
}
