/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_conversion.inc.c - Context adapters and mechanical HIR lowering
 */
#include "xxir_source_effect_bindings.inc.c"
static XrXirStatus source_conversion_intern(void *owner,const XrXirTypeNode *node,XrXirType *type) {
    SourceContext *ctx=owner;
    return source_intern_type(ctx,*node,type)?XR_XIR_OK:ctx->diagnostic.status;
}
static bool source_conversion_plan(SourceContext *ctx,AstNode *site,XrXirType actual,
    SourceExpectedType expected,XrHirConversion *output) {
    XrXirSourceRange range=source_query_range(ctx,site,NULL);
    XrHirConversionInput input={.origin={site?site->node_id:0,range.module,range.line,range.column,range.end_line,range.end_column},
        .actual=actual,.expected=expected.type,.has_expected=expected.present,
        .infer_result=expected.infer_result,.inferred=expected.inferred,
        .argument_bound=ctx->effect_argument && ctx->effect_argument->owner==ctx->function && ctx->effect_argument->syntax==site};
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    XrHirConversionContext context={&ctx->compile,&ctx->types,&proof,ctx->request,ctx,source_conversion_intern};
    const char *reason=NULL;XrXirStatus status=xr_hir_conversion_check(&context,&input,output,&reason);
    return status==XR_XIR_OK || source_fail(ctx,site,status,reason);
}
static bool source_conversion_emit(SourceContext *ctx,const XrHirConversion *conversion,SourceValue *value) {
    if(value->type!=conversion->source)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"conversion recipe source changed");
    if(conversion->kind==XR_HIR_CONVERSION_IDENTITY)return true;
    if(conversion->copy_nullable_element) {
        if(!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_COPY,conversion->source,{value->id,0},{0},0,{0}},value))return false;
        ctx->bodies[ctx->function].recipes[value->id-ctx->functions[ctx->function].parameter_count].root.kind=1;
    }
    if(!source_recipe_record(ctx,xr_hir_conversion_instruction(conversion,value->id),value))return false;
    if(conversion->root!=XR_HIR_CONVERSION_ROOT_NONE) {
        if(ctx->diagnostic.status!=XR_XIR_OK)return false;
        SourceRootValue *root=&ctx->bodies[ctx->function].recipes[value->id-ctx->functions[ctx->function].parameter_count].root;
        if(conversion->root==XR_HIR_CONVERSION_ROOT_ARGUMENT) {root->kind=5;root->declared=conversion->declared_bound;}
        else root->kind=conversion->root==XR_HIR_CONVERSION_ROOT_INFERRED?2:1;
    }
    return true;
}
