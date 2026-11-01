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
    if(signature.flags&XR_XIR_CALLABLE_REST) {
        plan.rest_count=count;plan.rest_array=UINT32_MAX;
        if(!count||signature.parameters[count-1].mode!=XR_PARAM_READ||!xr_xir_type_is_array(&ctx->types,parameters[count-1]))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"rest value has no final READ Array type");
        plan.rest_element=xr_xir_array_element(&ctx->types,parameters[count-1]);
    }
    plan.declaration.callable=callee.type;plan.declaration.callee_value=callee.id;
    if (!source_call_syntax(ctx,node,&syntax) || !source_call_expand(ctx,node,&syntax,&plan,count)) return false;
    if (plan.rest_count?source_call_rest_prefix(&plan,&syntax)!=count-1:(uint32_t)syntax.arg_count!=count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"indirect call requires its declared signature");
    if(plan.rest_count&&syntax.arg_count) {
        plan.values=source_recipe_storage(ctx,(uint32_t)syntax.arg_count,sizeof(*plan.values));
        if(!plan.values)return false;
    }
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    if(plan.rest_count&&!source_call_rest_pack(ctx,node,&plan,&syntax,arguments))return false;
    for(uint32_t p=0;p<count;++p) {
        if(!source_hir_call_use(ctx,node,&plan,(XrHirCallUseInput){.kind=XR_HIR_CALL_EXACT,.formal=p,
            .argument=p,.failure="indirect argument type mismatch"},&arguments[p],NULL))return false;
    }
    if (!source_query_call(ctx,UINT32_MAX,source_query_token_range(ctx,node->as.call_expr.callee))) return false;
    if(ctx->calls.count) {
        XrXirSourceType *shown=count?source_alloc(ctx,count,sizeof(*shown)):NULL;
        uint32_t *flags=plan.rest_count?source_alloc(ctx,count,sizeof(*flags)):NULL;
        if((count&&!shown)||(plan.rest_count&&!flags))return false;
        for(uint32_t p=0;p<count;++p) {
            if(!(p%64u)&&!source_request_poll(ctx,node))return false;
            XrXirType type=parameters[p];
            if(xr_xir_type_is_cell(&ctx->types,type))type=xr_xir_cell_element(&ctx->types,type);
            shown[p]=source_query_type(ctx,type);
        }
        if(flags)flags[count-1]=XR_XIR_SOURCE_CALL_VARIADIC;
        XrXirSourceCallSignature observed={.parameters=shown,.parameter_count=count,
            .result=source_query_type(ctx,signature.result),.known=true,.parameter_flags=flags,
            .callable=source_query_type(ctx,callee.type)};
        if(!source_direct_signature_names(ctx,node,&observed))return false;
        ((XrXirSourceCall *)ctx->calls.items)[ctx->calls.count-1].signature=observed;
    }
    if(!source_hir_call_commit(ctx,node,&plan,arguments,count,count))return false;
    return source_hir_call_group(ctx,plan.signature,(XrXirInstruction){XR_XIR_CALL_INDIRECT,signature.result,
        {0},{0},callee.id,{0}},arguments,value);
}
