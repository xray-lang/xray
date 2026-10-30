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
    SourceExpressionPlan *argument,const XrHirArrayCallbackShape *shape,SourceExpectedType *output) {
    *output=(SourceExpectedType){0};
    SourceExpressionPlan *original=source_call_argument_value_plan(argument);
    AstNode *literal=original->syntax;
    while (literal && literal->type==AST_GROUPING) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        literal=literal->as.grouping;
    }
    if (!literal || literal->type!=AST_FUNCTION_EXPR) return true;
    uint32_t count=(uint32_t)literal->as.function_expr.param_count;
    if (count<shape->minimum || count>shape->maximum)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array callback takes a different number of parameters");
    XrXirCallableParameter *parameters=source_alloc(ctx,count,sizeof(*parameters));
    if (!parameters) return false;
    for (uint32_t p=0;p<count;++p) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        parameters[p]=(XrXirCallableParameter){shape->parameters[p],XR_PARAM_READ};
    }
    XrXirType expected;
    if (!source_signature(ctx,parameters,count,shape->has_result ? shape->result : XR_XIR_UNIT,shape->error,&expected)) return false;
    *output=(SourceExpectedType){true,expected,!shape->has_result,false};return true;
}
static bool source_array_callback_arguments(SourceContext *ctx,AstNode *node,
    SourceArrayRecipe recipe,SourceValue array,SourceArrayCallback *callback,SourceValue *initial) {
    uint32_t count=recipe==SOURCE_ARRAY_REDUCE ? 2 : 1;
    XrXirType parameters[2]={XR_XIR_UNIT,XR_XIR_UNIT};
    SourceExpectedType contexts[2]={{0},{0}};SourceValue values[2]={{0}, {0}};
    SourceCallPlan plan={.family=SOURCE_CALL_ARRAY,.prefix={0},.substitution={0},.function_parameters=parameters,.requirement_parameters=NULL,.result=XR_XIR_UNIT,
        .expected_result={0},.values=values,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=contexts};
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method requires its declared arguments");
    if (!source_call_expand(ctx,node,&syntax,&plan,count)) return false;
    if ((uint32_t)syntax.arg_count!=count || !syntax.expanded)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"Array method requires its declared arguments");
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
    }
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_ARRAY);
    const XrNativeMemberDeclaration *member=source_array_member(ctx,node,node->as.call_expr.callee->as.member_access.name);
    if (!member || source_array_recipe(member->operation)!=recipe ||
        !source_native_callback_error(ctx,node,native,member,&plan.declaration.callback_error)) return false;
    plan.declaration.array=array.type;plan.declaration.array_value=array.id;
    plan.declaration.accumulator=parameters[1];plan.declaration.array_member=member;
    if(!source_hir_call_prepare(ctx,node,&plan,count))return false;
    XrHirArrayCallbackShape shape;
    if(xr_hir_body_array_context(ctx->typed_body,plan.signature,array.type,array.id,&shape)!=XR_XIR_OK)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"Array callback context has no actual receiver");
    if (!source_array_callback_context(ctx,node,syntax.expanded->entries[0].plan.value,&shape,&contexts[0])) return false;
    for(uint32_t a=0;a<count;++a) {
        SourceExpressionPlan *argument=syntax.expanded->entries[a].plan.value;
        if(argument&&argument->call_projection)argument->call_projection->signature=plan.signature;
    }
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    for(uint32_t p=0;p<count;++p) {
        XrHirBodyAction actual;
        XrHirCallUseInput input={.kind=XR_HIR_CALL_FINAL,.formal=p,.argument=p,
            .check_type=p!=0,.expected=parameters[p]};
        if(!source_hir_call_use(ctx,node,&plan,input,&values[p],&actual))return false;
        if(!p)callback->arity=actual.argument_count;
    }
    if(!source_hir_call_native_finish(ctx,&plan))return false;
    callback->value=values[0];*initial=count==2 ? values[1] : (SourceValue){0};return true;
}
