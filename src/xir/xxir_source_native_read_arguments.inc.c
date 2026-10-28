/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_native_read_arguments.inc.c - Ordinary native READ call arguments
 *
 * KEY CONCEPT: Governed recipes consume the same authentic ordered call plans.
 */
static bool source_native_read_arguments(SourceContext *ctx,AstNode *node,
    const XrXirType *parameters,uint32_t minimum,uint32_t maximum,SourceValue *values,
    uint32_t *supplied,const char *diagnostic) {
    if (!node || node->type!=AST_CALL_EXPR || minimum>maximum ||
        (maximum && (!parameters || !values)) || !supplied)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native READ parameter vector is inconsistent");
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    for (uint32_t p=0;p<maximum;++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (xr_xir_type_is_cell(&ctx->types,parameters[p]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"native READ arguments cannot grant a ref contract");
    }
    /* Native parameter types are already instantiated in the caller's pool.
     * Their shared READ plan preserves those binders without applying a
     * declaration callee's type substitution. */
    SourceCallPlan plan={SOURCE_CALL_NATIVE,{0},{0},parameters,NULL,XR_XIR_UNIT,
        {0},values,0,0,NULL,NULL};
    if (!source_call_expand(ctx,&syntax,&plan,maximum)) return false;
    if ((uint32_t)syntax.arg_count<minimum || (uint32_t)syntax.arg_count>maximum)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    *supplied=(uint32_t)syntax.arg_count;
    return true;
}
static bool source_native_variadic_read_arguments(SourceContext *ctx,AstNode *node,
    XrXirType parameter,SourceValue **values,uint32_t *supplied,const char *diagnostic) {
    if (!values || !supplied || xr_xir_type_is_cell(&ctx->types,parameter))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native variadic READ parameter is inconsistent");
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count) return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    SourceCallPlan plan={SOURCE_CALL_VARIADIC_READ,{0},{0},&parameter,NULL,XR_XIR_UNIT,
        {0},NULL,0,0,NULL,NULL};
    if (!source_call_expand(ctx,&syntax,&plan,65536)) return false;
    uint32_t count=(uint32_t)syntax.arg_count;
    plan.values=count ? source_recipe_storage(ctx,count,sizeof(*plan.values)) : NULL;
    if (count && !plan.values) return false;
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    *values=plan.values;*supplied=count;
    return true;
}
