/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_indirect_call.inc.c - Signature-owned ordinary argument plans
 */
static bool source_indirect_call(SourceContext *ctx,AstNode *node,SourceValue callee,SourceValue *value) {
    const XrXirTypeNode *found=xr_xir_callable_signature(&ctx->types,callee.type);
    if (!found) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect call requires its declared signature");
    XrXirTypeNode signature=*found;uint32_t count=signature.parameter_count;
    XrXirType *parameters=count ? source_alloc(ctx,count,sizeof(*parameters)) : NULL;
    SourceValue *arguments=count ? source_recipe_storage(ctx,count,sizeof(*arguments)) : NULL;
    if (count && (!parameters || !arguments)) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!source_work(ctx,node)) return false;
        if (!xr_xir_callable_parameter_storage_valid(&ctx->types,&signature.parameters[p]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect parameter storage and mode disagree");
        parameters[p]=signature.parameters[p].type;
    }
    SourceCallSyntax syntax;
    SourceCallPlan plan={SOURCE_CALL_INDIRECT,{0},{0},parameters,NULL,signature.result,{0},arguments,0,0,NULL};
    if (!source_call_syntax(ctx,node,&syntax) || !source_call_expand(ctx,node,&syntax,&plan,count)) return false;
    if ((uint32_t)syntax.arg_count!=count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect call requires its declared signature");
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax)) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!source_work(ctx,node)) return false;
        if (arguments[p].type!=parameters[p]) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect argument type mismatch");
    }
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL_INDIRECT,signature.result,
        {0},{0},callee.id,{0}},arguments,count,value);
}
