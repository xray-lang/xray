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
    /* Native parameter types are already instantiated in the caller's pool.
     * Their shared READ plan preserves those binders without applying a
     * declaration callee's type substitution. */
    SourceCallPlan plan={.family=SOURCE_CALL_NATIVE,.prefix={0},.substitution={0},.function_parameters=parameters,.requirement_parameters=NULL,.result=XR_XIR_UNIT,
        .expected_result={0},.values=values,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=NULL};
    if (!source_call_expand(ctx,node,&syntax,&plan,maximum)) return false;
    if ((uint32_t)syntax.arg_count<minimum || (uint32_t)syntax.arg_count>maximum)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    if(!source_hir_call_commit(ctx,node,&plan,values,(uint32_t)syntax.arg_count,(uint32_t)syntax.arg_count)||
        !source_hir_call_native_finish(ctx,&plan))return false;
    *supplied=(uint32_t)syntax.arg_count;
    return true;
}
static bool source_native_variadic_read_arguments(SourceContext *ctx,AstNode *node,
    XrXirType parameter,SourceValue **values,uint32_t *supplied,const char *diagnostic) {
    if (!values || !supplied)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native variadic READ parameter is inconsistent");
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count) return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    SourceCallPlan plan={.family=SOURCE_CALL_VARIADIC_READ,.prefix={0},.substitution={0},.function_parameters=&parameter,.requirement_parameters=NULL,.result=XR_XIR_UNIT,
        .expected_result={0},.values=NULL,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=NULL};
    if (!source_call_expand(ctx,node,&syntax,&plan,65536)) return false;
    uint32_t count=(uint32_t)syntax.arg_count;
    plan.values=count ? source_recipe_storage(ctx,count,sizeof(*plan.values)) : NULL;
    if (count && !plan.values) return false;
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    if(!source_hir_call_commit(ctx,node,&plan,plan.values,count,count)||!source_hir_call_native_finish(ctx,&plan))return false;
    *values=plan.values;*supplied=count;
    return true;
}
