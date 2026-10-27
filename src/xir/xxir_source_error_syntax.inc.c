/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_error_syntax.inc.c - Reject error syntax without checked semantics
 */
typedef struct SourceErrorSyntax {
    SourceContext *ctx;
    AstNode *node;
} SourceErrorSyntax;

static bool source_error_syntax_fail(SourceErrorSyntax *scan, int line, int column,
                                     const char *message) {
    bool fresh = scan->ctx->diagnostic.status == XR_XIR_OK;
    source_fail(scan->ctx, scan->node, XR_XIR_BAD_STRUCTURE, message);
    if (fresh && line > 0 && column > 0) {
        scan->ctx->diagnostic.line = line;
        scan->ctx->diagnostic.column = column;
    }
    return false;
}

static bool source_error_clause(SourceErrorSyntax *scan, const XrThrowsClause *clause) {
    if (clause->syntax == XR_THROWS_OMITTED || clause->syntax == XR_THROWS_ERASED ||
        (clause->syntax == XR_THROWS_TYPED && clause->type)) return true;
    return source_error_syntax_fail(scan, clause->line, clause->column,
        "throws clause is not yet supported by the compiler");
}

static bool source_error_type(const XrTypeRef *type, void *pointer) {
    SourceErrorSyntax *scan = pointer;
    if (!source_work(scan->ctx, scan->node) || !source_error_clause(scan, &type->throws_clause))
        return false;
    return scan->ctx->diagnostic.status == XR_XIR_OK;
}

/* Declaration inventory visits unreachable bodies and unused declarations too.
 * Reject before semantic construction so no consumer can erase a new clause. */
static bool source_error_syntax(SourceContext *ctx, AstNode *node) {
    SourceErrorSyntax scan = {ctx, node};
    const XrThrowsClause *clause = NULL;
    switch (node->type) {
    case AST_FUNCTION_DECL: case AST_FUNCTION_EXPR:
        clause = &node->as.function_decl.throws_clause; break;
    case AST_METHOD_DECL:
        clause = &node->as.method_decl.throws_clause; break;
    case AST_INTERFACE_METHOD:
        clause = &node->as.interface_method.throws_clause; break;
    case AST_TRY_EXPR:
        if (node->as.try_expr.mode == XR_TRY_PROPAGATE) return true;
        return source_error_syntax_fail(&scan, node->line, node->column,
            node->as.try_expr.mode == XR_TRY_OPTIONAL ? "try? expressions are not yet supported by the compiler" :
            node->as.try_expr.mode == XR_TRY_ASSERT ? "try! expressions are not yet supported by the compiler" :
            "try expressions are not yet supported by the compiler");
    default: break;
    }
    if (clause && !source_error_clause(&scan, clause)) return false;
    if (!xr_ast_for_each_type_ref(node, source_error_type, &scan))
        return ctx->diagnostic.status != XR_XIR_OK ? false :
            source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "error syntax type traversal failed");
    return true;
}
