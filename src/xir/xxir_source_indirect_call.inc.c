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
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        parameters[p]=signature.parameters[p].type;
    }
    SourceCallSyntax syntax;
    SourceCallPlan plan={.family=SOURCE_CALL_INDIRECT,.prefix={0},.substitution={0},.function_parameters=parameters,.requirement_parameters=NULL,.result=signature.result,.expected_result={0},.values=arguments,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=NULL};
    plan.declaration.callable=callee.type;plan.declaration.callee_value=callee.id;
    if (!source_call_syntax(ctx,node,&syntax) || !source_call_expand(ctx,node,&syntax,&plan,count)) return false;
    if ((uint32_t)syntax.arg_count!=count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect call requires its declared signature");
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    for(uint32_t p=0;p<count;++p) {
        if(!source_hir_call_use(ctx,node,&plan,(XrHirCallUseInput){.kind=XR_HIR_CALL_EXACT,.formal=p,
            .argument=p,.failure="indirect argument type mismatch"},&arguments[p],NULL))return false;
    }
    if (!source_query_call(ctx,UINT32_MAX,source_query_token_range(ctx,node->as.call_expr.callee))) return false;
    if(!source_hir_call_commit(ctx,node,&plan,arguments,count,count))return false;
    return source_hir_call_group(ctx,plan.signature,(XrXirInstruction){XR_XIR_CALL_INDIRECT,signature.result,
        {0},{0},callee.id,{0}},arguments,value);
}
