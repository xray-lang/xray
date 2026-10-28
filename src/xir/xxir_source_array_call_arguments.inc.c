/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_array_call_arguments.inc.c - Ordered contextual Array READ calls
 *
 * KEY CONCEPT: A closure header receives context without evaluating its captures.
 */
static bool source_array_callback_context(SourceContext *ctx,AstNode *node,
    SourceExpressionPlan *argument,const SourceArrayCallbackShape *shape,SourceExpectedType *output) {
    *output=(SourceExpectedType){0};
    SourceExpressionPlan *original=source_call_argument_value_plan(argument);
    AstNode *literal=original->syntax;
    while (literal && literal->type==AST_GROUPING) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        literal=literal->as.grouping;
    }
    if (!literal || literal->type!=AST_FUNCTION_EXPR) return true;
    uint32_t count=(uint32_t)literal->as.function_expr.param_count;
    if (count<shape->shortest || count>shape->longest)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array callback takes a different number of parameters");
    XrXirCallableParameter *parameters=source_alloc(ctx,count,sizeof(*parameters));
    if (!parameters) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        parameters[p]=(XrXirCallableParameter){shape->parameters[p],XR_PARAM_READ};
    }
    XrXirType expected;
    if (!source_signature(ctx,parameters,count,shape->result.present ? shape->result.type : XR_XIR_UNIT,shape->error_type,&expected)) return false;
    *output=(SourceExpectedType){true,expected,!shape->result.present,false};return true;
}
static bool source_array_callback_check(SourceContext *ctx,AstNode *node,
    const SourceArrayCallbackShape *shape,SourceArrayCallback *output) {
    const XrXirTypeNode *signature=xr_xir_callable_signature(&ctx->types,output->value.type);
    if (!signature || signature->parameter_count<shape->shortest || signature->parameter_count>shape->longest)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array callback requires a function of the declared shape");
    output->arity=signature->parameter_count;
    if (!xr_xir_error_bound_accepts(&ctx->types,signature->element,shape->error_type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array callback error exceeds its declaration");
    for (uint32_t p=0;p<signature->parameter_count;++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (signature->parameters[p].type!=shape->parameters[p] || signature->parameters[p].mode!=XR_PARAM_READ)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array callback parameter type differs from the element");
    }
    if (shape->result.present && signature->result!=shape->result.type)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array callback result differs from its declared result");
    return true;
}
static bool source_array_callback_arguments(SourceContext *ctx,AstNode *node,
    SourceArrayRecipe recipe,SourceValue array,SourceArrayCallback *callback,SourceValue *initial) {
    uint32_t count=recipe==SOURCE_ARRAY_REDUCE ? 2 : 1;
    XrXirType parameters[2]={XR_XIR_UNIT,XR_XIR_UNIT};
    SourceExpectedType contexts[2]={{0},{0}};SourceValue values[2]={{0},{0}};
    SourceCallPlan plan={SOURCE_CALL_INDIRECT,{0},{0},parameters,NULL,XR_XIR_UNIT,
        {0},values,0,0,NULL,contexts};
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method requires its declared arguments");
    if (!source_call_expand(ctx,&syntax,&plan,count)) return false;
    if ((uint32_t)syntax.arg_count!=count || !syntax.expanded)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method requires its declared arguments");
    XrXirType element=xr_xir_array_element(&ctx->types,array.type);
    bool indexed=recipe==SOURCE_ARRAY_MAP || recipe==SOURCE_ARRAY_FILTER || recipe==SOURCE_ARRAY_FOR_EACH;
    SourceArrayCallbackShape shape={{element,XR_XIR_I64},1,indexed ? 2 : 1,
        recipe==SOURCE_ARRAY_MAP ? (SourceExpectedType){0} :
        (SourceExpectedType){true,recipe==SOURCE_ARRAY_FOR_EACH ? XR_XIR_UNIT : XR_XIR_BOOL,false,false},XR_XIR_UNIT};
    if (recipe==SOURCE_ARRAY_REDUCE) {
        SourceExpressionPlan *argument=source_call_argument_value_plan(syntax.expanded->entries[1].plan.value);
        if (!source_plan_binary_prepare(ctx,argument,(SourceExpectedType){0},true) ||
            !source_plan_numeric_prepare(ctx,argument,(SourceExpectedType){0},true)) return false;
        if (!argument->type_ready)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"reduce initial type is not admitted without a known context");
        XrXirType accumulator=argument->ground_type;
        for (uint32_t claim=0;claim<argument->read_claims && xr_xir_type_is_nullable(&ctx->types,accumulator);++claim) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            accumulator=xr_xir_nullable_element(&ctx->types,accumulator);
        }
        if (accumulator==XR_XIR_UNIT)
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"reduce initial value cannot be unit");
        parameters[1]=accumulator;contexts[1]=(SourceExpectedType){true,accumulator,false,false};
        shape=(SourceArrayCallbackShape){{accumulator,element},2,2,{true,accumulator,false,false},XR_XIR_UNIT};
    }
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=source_array_member(ctx,node,node->as.call_expr.callee->as.member_access.name);
    if (!member || source_array_recipe(member->operation)!=recipe ||
        !source_native_callback_error(ctx,node,native,member,&shape.error_type)) return false;
    if (!source_array_callback_context(ctx,node,syntax.expanded->entries[0].plan.value,&shape,&contexts[0])) return false;
    parameters[0]=contexts[0].type;
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    callback->value=values[0];*initial=count==2 ? values[1] : (SourceValue){0};
    return source_array_callback_check(ctx,node,&shape,callback);
}
