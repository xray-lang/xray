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
    if (count && !parameters) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!source_work(ctx,node)) return false;
        if (!xr_xir_callable_parameter_storage_valid(&ctx->types,&signature.parameters[p]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect parameter storage and mode disagree");
        parameters[p]=signature.parameters[p].type;
    }
    SourceCallSyntax syntax;
    SourceCallPlan plan={SOURCE_CALL_INDIRECT,{0},{0},parameters,NULL,signature.result,{0},NULL,0,0,NULL,NULL,0,0};
    if (signature.flags&XR_XIR_CALLABLE_VARIADIC) {
        if (!count || !xr_xir_type_is_array(&ctx->types,parameters[count-1]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect rest signature requires an Array tail");
        plan.rest_parameter=count;
        plan.rest_element=xr_xir_array_element(&ctx->types,parameters[count-1]);
    }
    if (!source_call_syntax(ctx,node,&syntax) || !source_call_expand(ctx,node,&syntax,&plan,count)) return false;
    uint32_t supplied=(uint32_t)syntax.arg_count;
    if (plan.rest_parameter ? supplied<count-1 : supplied!=count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect call requires its declared signature");
    uint32_t capacity=supplied>count ? supplied : count;
    SourceValue *arguments=capacity ? source_recipe_storage(ctx,capacity,sizeof(*arguments)) : NULL;
    if (capacity && !arguments) return false;
    plan.values=arguments;
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax)) return false;
    SourceCallConversions conversions={SOURCE_CALL_INDIRECT,{0},parameters,NULL,arguments,supplied,0,
        plan.rest_parameter,plan.rest_element};
    if (!source_call_conversions(ctx,node,&conversions) || !source_call_rest_pack(ctx,node,&plan,supplied)) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!source_work(ctx,node)) return false;
        if (arguments[p].type!=parameters[p]) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect argument type mismatch");
    }
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL_INDIRECT,signature.result,
        {0},{0},callee.id,{0}},arguments,count,value);
}
