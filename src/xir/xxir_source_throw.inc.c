/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_throw.inc.c - Preserve actual throws before local error routing
 */
static bool source_throw(SourceContext *ctx,AstNode *node) {
    SourceValue value={0};
    if (!expression(ctx,node->as.throw_stmt.expression,&value)) return false;
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext context={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    XrXirStatus status=xr_xir_compile_type_markers_prove(&ctx->compile,&context,value.type,XR_XIR_CONSTRAINT_ERROR);
    if (status!=XR_XIR_OK) return source_fail(ctx,node,status,"throw requires a proved enum error value");
    if (ctx->bodies[ctx->function].error_context) {
        SourceConversionRecipe conversion;
        if (!source_conversion_plan(ctx,node,value.type,(SourceExpectedType){true,XR_XIR_ERROR,false,false},&conversion) ||
            !source_conversion_emit(ctx,&conversion,&value) || !source_error_edge(ctx,value)) return false;
    } else {
        ctx->returned=true;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{value.id,0},{0,0},0,{0}},NULL)) return false;
    }
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (!body->count || body->throw_count==UINT32_MAX)
        return source_fail(ctx,node,XR_XIR_BUDGET,"source throw observation exhausted");
    SourceThrowRecipe *site=source_alloc(ctx,1,sizeof(*site));if (!site) return false;
    *site=(SourceThrowRecipe){body->throw_recipes,body->count-1,value.id};
    body->throw_recipes=site;++body->throw_count;ctx->returned=true;return true;
}
