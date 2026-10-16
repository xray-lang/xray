/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_function_inventory.inc.c - Complete nested Source function inventory
 */
typedef struct SourceClosureCount { SourceContext *ctx; uint32_t count, depth, defaults, members, comptime; bool yielded; } SourceClosureCount;
static bool count_closures(AstNode *node, void *pointer) {
    SourceClosureCount *scan = pointer;
    if (!node) return true;
    if (!source_work(scan->ctx,node)) return false;
    /* A module-level declaration is a direct child of the program node, at depth one. */
    bool closure = node->type == AST_FUNCTION_EXPR || node->type == AST_DEFER_STMT ||
        (node->type == AST_FUNCTION_DECL && scan->depth > 1);
    if (scan->depth == 128 || (closure && scan->count == scan->ctx->compile.limits.functions))
        return source_fail(scan->ctx,node,XR_XIR_BUDGET,"closure declaration budget exhausted");
    if (closure) ++scan->count;
    bool function=node->type==AST_FUNCTION_EXPR || node->type==AST_FUNCTION_DECL || node->type==AST_METHOD_DECL;
    bool outer_yield=scan->yielded;if(function) scan->yielded=false;
    if(node->type==AST_YIELD_STMT) scan->yielded=true;
    if (node->type==AST_COMPTIME_EXPR) {
        if (scan->comptime==scan->ctx->compile.limits.functions)
            return source_fail(scan->ctx,node,XR_XIR_BUDGET,"comptime helper capacity exhausted");
        ++scan->comptime;
    }
    if (node->type == AST_MEMBER_ACCESS) {
        if (scan->members == UINT32_MAX)
            return source_fail(scan->ctx,node,XR_XIR_BUDGET,"member helper capacity exhausted");
        ++scan->members;
    }
    XrParamNode **parameters = node->type == AST_FUNCTION_DECL ? node->as.function_decl.params :
        node->type == AST_METHOD_DECL ? node->as.method_decl.params : NULL;
    int count = node->type == AST_FUNCTION_DECL ? node->as.function_decl.param_count :
        node->type == AST_METHOD_DECL ? node->as.method_decl.param_count : 0;
    for (int p = 0; p < count; ++p) {
        if (!source_work(scan->ctx, node)) return false;
        if (parameters[p]->default_value) {
            if (scan->defaults == scan->ctx->compile.limits.functions)
                return source_fail(scan->ctx, node, XR_XIR_BUDGET, "default argument function budget exhausted");
            ++scan->defaults;
        }
    }
    ++scan->depth;
    bool ok = xr_ast_for_each_child(node,count_closures,scan);
    --scan->depth;
    if(closure && function && scan->yielded) {
        if(scan->count==scan->ctx->compile.limits.functions)
            return source_fail(scan->ctx,node,XR_XIR_BUDGET,"generator factory inventory exhausted");
        ++scan->count;
    }
    if(function) scan->yielded=outer_yield;
    return ok || source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"unknown source declaration shape");
}
/* Code after return, throw, break or continue never runs and has no control-flow edge, so it is dropped.
 * Closures in it are rejected because their declarations are counted ahead of lowering and must each be lowered. */
static bool source_unreachable_statement(SourceContext *ctx, AstNode *node) {
    SourceClosureCount scan = {ctx, 0, 2, 0, 0, 0, false};
    if (!count_closures(node, &scan)) return false;
    if (scan.count || scan.comptime) return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "closures or comptime in unreachable statements are not admitted");
    return true;
}
static bool source_comptime_inventory(SourceContext *ctx,uint32_t count,uint32_t *capacity) {
    if (!count) return true;
    if (count>UINT32_MAX-*capacity)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"comptime function capacity overflow");
    if (!source_work_units(ctx,NULL,sizeof(*capacity)+sizeof(ctx->comptime_capacity))) return false;
    *capacity+=count;ctx->comptime_capacity=count;return true;
}
