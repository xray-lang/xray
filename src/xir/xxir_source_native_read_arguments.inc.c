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
typedef struct SourceNativeReadInput {
    const XrXirType *parameters;
    uint32_t minimum,maximum,native_owner;
    SourceValue *values;
    uint32_t *supplied;
    bool *present;
    const char *diagnostic;
    SourceCallDisplayBinding *display;
} SourceNativeReadInput;
static bool source_native_read_declaration(SourceContext *ctx,AstNode *node,uint32_t identity,SourceCallPlan *plan) {
    if(!identity)return true;
    AstNode *callee=node->as.call_expr.callee;
    const XrNativeTypeDeclaration *owner=xr_native_declaration_by_id(identity);
    const XrNativeMemberDeclaration *member=callee&&callee->type==AST_MEMBER_ACCESS?
        source_native_member(ctx,owner,callee->as.member_access.name):NULL;
    if(!member)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native READ call has no actual declaration");
    plan->declaration.native_owner=identity;plan->declaration.native_member=member;return true;
}
static bool source_native_read_arguments(SourceContext *ctx,AstNode *node,const SourceNativeReadInput *input) {
    if(!input)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native READ input is missing");
    const XrXirType *parameters=input->parameters;uint32_t minimum=input->minimum,maximum=input->maximum;
    SourceValue *values=input->values;uint32_t *supplied=input->supplied;const char *diagnostic=input->diagnostic;
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
    if(!source_native_read_declaration(ctx,node,input->native_owner,&plan))return false;
    if (!source_call_expand(ctx,node,&syntax,&plan,maximum)) return false;
    if ((uint32_t)syntax.arg_count<minimum || (uint32_t)syntax.arg_count>maximum)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    for(uint32_t p=0;p<maximum;++p) {
        bool present=plan.named_arguments?plan.binding.formal_arguments[p]!=UINT32_MAX:p<(uint32_t)syntax.arg_count;
        if(input->present)input->present[p]=present;
        if(!present)continue;
        XrHirCallUseInput final={.kind=XR_HIR_CALL_FINAL,.formal=p,.argument=source_call_final_argument(&plan,p),
            .check_type=true,.expected=parameters[p]};
        if(!source_hir_call_use(ctx,node,&plan,final,&values[p],NULL))return false;
    }
    if(!source_call_display_complete(ctx,node,&plan)||!source_hir_call_native_finish(ctx,&plan))return false;
    if(input->display)*input->display=(SourceCallDisplayBinding){plan.display_argument_formals,
        plan.display_argument_count,plan.display_argument_binding_known};
    *supplied=(uint32_t)syntax.arg_count;
    return true;
}
typedef struct SourceNativeVariadicReadInput {
    XrXirType parameter;
    uint32_t native_owner;
    SourceValue **values;
    uint32_t *supplied;
    const char *diagnostic;
} SourceNativeVariadicReadInput;
static bool source_native_variadic_read_arguments(SourceContext *ctx,AstNode *node,const SourceNativeVariadicReadInput *input) {
    if(!input)return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native variadic READ input is missing");
    XrXirType parameter=input->parameter;SourceValue **values=input->values;
    uint32_t *supplied=input->supplied;const char *diagnostic=input->diagnostic;
    if (!values || !supplied)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"native variadic READ parameter is inconsistent");
    SourceCallSyntax syntax;
    if (!source_call_syntax(ctx,node,&syntax)) return false;
    if (syntax.type_arg_count) return source_fail(ctx,node,XR_XIR_BAD_TYPE,diagnostic);
    SourceCallPlan plan={.family=SOURCE_CALL_VARIADIC_READ,.prefix={0},.substitution={0},.function_parameters=&parameter,.requirement_parameters=NULL,.result=XR_XIR_UNIT,
        .expected_result={0},.values=NULL,.parameter_offset=0,.value_offset=0,.parameter_kinds=NULL,.argument_contexts=NULL};
    if(!source_native_read_declaration(ctx,node,input->native_owner,&plan))return false;
    if (!source_call_expand(ctx,node,&syntax,&plan,65536)) return false;
    uint32_t count=(uint32_t)syntax.arg_count;
    plan.values=count ? source_recipe_storage(ctx,count,sizeof(*plan.values)) : NULL;
    if (count && !plan.values) return false;
    if (!source_call_plan_arguments(ctx,node,&plan,&syntax,NULL)) return false;
    if(!source_hir_call_commit(ctx,node,&plan,plan.values,count,count)||!source_hir_call_native_finish(ctx,&plan))return false;
    *values=plan.values;*supplied=count;
    return true;
}
