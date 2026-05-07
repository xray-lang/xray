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
 *   Currently a skeleton (identity pass) — individual canonicalization
 *   rules are added incrementally.
 */

#include "xcanon.h"
#include "../../base/xchecks.h"
#include "../../base/xdefs.h"
#include "../parser/xast_nodes.h"
#include "../analyzer/xanalyzer.h"

#include <string.h>

/* ========== Canonicalizer context ========== */

typedef struct XrCanonCtx {
    struct XaAnalyzer *analyzer;
    struct XrayIsolate *isolate;
    int error_count;
} XrCanonCtx;

/* ========== AST walk (recursive, dispatches on node type) ========== */

/* Forward declaration — the walker calls itself recursively. */
static void canon_node(XrCanonCtx *ctx, AstNode *node);

/* Canonicalize a block's statements in order. */
static void canon_block(XrCanonCtx *ctx, AstNode **stmts, int count) {
    XR_DCHECK(ctx != NULL, "canon_block: NULL ctx");
    for (int i = 0; i < count; i++) {
        if (stmts[i]) canon_node(ctx, stmts[i]);
    }
}

/* Recursive canonicalization dispatcher.
 * Currently a no-op walk — each canonicalization rule will add its
 * handling to the appropriate case. */
static void canon_node(XrCanonCtx *ctx, AstNode *node) {
    if (!node) return;

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
            if (m->arms[i]) canon_node(ctx, m->arms[i]);
        }
        break;
    }

    case AST_MATCH_ARM:
        canon_node(ctx, node->as.match_arm.body);
        break;

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
            if (call->arguments[i]) canon_node(ctx, call->arguments[i]);
        }
        break;
    }

    case AST_MEMBER_ACCESS:
        canon_node(ctx, node->as.member_access.object);
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

    /* ---- Binary / unary expressions ---- */
    case AST_BINARY_ADD: case AST_BINARY_SUB: case AST_BINARY_MUL:
    case AST_BINARY_DIV: case AST_BINARY_MOD: case AST_BINARY_EQ:
    case AST_BINARY_NE:  case AST_BINARY_LT:  case AST_BINARY_LE:
    case AST_BINARY_GT:  case AST_BINARY_GE:  case AST_BINARY_AND:
    case AST_BINARY_OR:  case AST_BINARY_BAND: case AST_BINARY_BOR:
    case AST_BINARY_BXOR: case AST_BINARY_LSHIFT: case AST_BINARY_RSHIFT:
    case AST_NULLISH_COALESCE: case AST_RANGE:
        canon_node(ctx, node->as.binary.left);
        canon_node(ctx, node->as.binary.right);
        break;

    case AST_UNARY_NEG: case AST_UNARY_NOT: case AST_UNARY_BNOT:
        canon_node(ctx, node->as.unary.operand);
        break;

    /* ---- Compound assignment (future: expand to load-op-store) ---- */
    case AST_COMPOUND_ASSIGNMENT:
        canon_node(ctx, node->as.compound_assignment.object);
        canon_node(ctx, node->as.compound_assignment.value);
        break;

    /* ---- Leaf nodes and cases handled by identity pass ---- */
    default:
        /* Literals, variables, break, continue, etc. — nothing to walk. */
        break;
    }
}

/* ========== Public API ========== */

XR_FUNC XrCanonStatus xr_canon_program(struct AstNode *program,
                                        struct XaAnalyzer *analyzer,
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

XR_FUNC XrCanonStatus xr_canon_func(struct AstNode *func_node,
                                     struct XaAnalyzer *analyzer,
                                     struct XrayIsolate *isolate) {
    if (!func_node || !analyzer || !isolate)
        return XR_CANON_ERR_NULL_INPUT;

    XR_DCHECK(func_node->type == AST_FUNCTION_DECL ||
              func_node->type == AST_FUNCTION_EXPR,
              "xr_canon_func: expected function node");

    XrCanonCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.analyzer = analyzer;
    ctx.isolate = isolate;

    canon_node(&ctx, func_node);

    return ctx.error_count > 0 ? XR_CANON_ERR_INTERNAL : XR_CANON_OK;
}
