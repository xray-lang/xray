/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_generator.inc.c - Generator roles from the real lexical body
 *
 * KEY CONCEPT:
 *   Nested function bodies own their yields; analyzer flags grant no role.
 */
typedef struct SourceGeneratorScan {SourceContext *ctx;uint32_t depth;bool found;} SourceGeneratorScan;
static bool source_generator_scan(AstNode *node,void *opaque) {
    SourceGeneratorScan *scan=opaque;
    if (!node) return true;
    if (!source_work(scan->ctx,node)) return false;
    if (node->type==AST_FUNCTION_DECL || node->type==AST_FUNCTION_EXPR) return true;
    if (node->type==AST_YIELD_STMT) scan->found=true;
    if (scan->depth>=128) return source_fail(scan->ctx,node,XR_XIR_BUDGET,"generator body depth exhausted");
    ++scan->depth;
    bool ok=xr_ast_for_each_child(node,source_generator_scan,scan);
    --scan->depth;
    return ok || source_fail(scan->ctx,node,XR_XIR_BAD_STRUCTURE,"generator body structure is not admitted");
}
static bool source_generator_declaration(SourceContext *ctx,AstNode *node,uint32_t index,
    XrXirFunctionIdentity *identity) {
    const XrXirFunction *function=&ctx->functions[index];
    const XrXirTypeNode *result=xr_xir_type_node(&ctx->types,function->result);
    if (!result || result->kind!=XR_XIR_TYPE_ITERATOR) return true;
    SourceGeneratorScan scan={ctx,0,false};
    if (!source_generator_scan(node->as.function_decl.body,&scan)) return false;
    if (!scan.found) return true;
    if (identity->test_role || identity->cleanup_owner)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"generator cannot be a test or cleanup body");
    for (uint32_t p=0;p<function->parameter_count;++p) {
        if (!source_work(ctx,node)) return false;
        if (xr_xir_type_is_cell(&ctx->types,function->parameters[p]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"generator cannot retain an external ref parameter");
    }
    identity->promises|=XR_XIR_FUNCTION_GENERATOR;
    return true;
}
static bool source_generator_yield(SourceContext *ctx,AstNode *node,bool top) {
    if (top || !(ctx->identities[ctx->function].promises & XR_XIR_FUNCTION_GENERATOR) ||
        ctx->identities[ctx->function].cleanup_owner)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"yield requires its own Iterator-result generator body");
    XrXirType element=xr_xir_iterator_element(&ctx->types,ctx->functions[ctx->function].result);
    SourceValue value={0};
    if (!node->as.yield_stmt.value || !source_plan_expression(ctx,node->as.yield_stmt.value,
        (SourceExpectedType){true,element,false,false},&value)) return false;
    if (value.type!=element) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"yield element differs from Iterator result");
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_GENERATOR_YIELD,XR_XIR_UNIT,
        {element==XR_XIR_UNIT ? 0 : value.id,0},{0},0,{0}},NULL);
}
