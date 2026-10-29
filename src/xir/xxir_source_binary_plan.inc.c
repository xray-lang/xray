/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_binary_plan.inc.c - Syntax adapters and mechanical binary lowering
 */
static bool source_binary_operator(AstNodeType syntax,XrHirBinaryOperator *operation) {
    switch(syntax) {
    case AST_BINARY_ADD:*operation=XR_HIR_BINARY_ADD;break;
    case AST_BINARY_SUB:*operation=XR_HIR_BINARY_SUB;break;
    case AST_BINARY_MUL:*operation=XR_HIR_BINARY_MUL;break;
    case AST_BINARY_DIV:*operation=XR_HIR_BINARY_DIV;break;
    case AST_BINARY_MOD:*operation=XR_HIR_BINARY_REM;break;
    case AST_BINARY_BAND:*operation=XR_HIR_BINARY_AND;break;
    case AST_BINARY_BOR:*operation=XR_HIR_BINARY_OR;break;
    case AST_BINARY_BXOR:*operation=XR_HIR_BINARY_XOR;break;
    case AST_BINARY_LSHIFT:*operation=XR_HIR_BINARY_SHL;break;
    case AST_BINARY_RSHIFT:*operation=XR_HIR_BINARY_SHR;break;
    case AST_BINARY_EQ:*operation=XR_HIR_BINARY_EQ;break;
    case AST_BINARY_NE:*operation=XR_HIR_BINARY_NE;break;
    case AST_BINARY_LT:*operation=XR_HIR_BINARY_LT;break;
    case AST_BINARY_LE:*operation=XR_HIR_BINARY_LE;break;
    case AST_BINARY_GT:*operation=XR_HIR_BINARY_GT;break;
    case AST_BINARY_GE:*operation=XR_HIR_BINARY_GE;break;
    default:return false;
    }
    return true;
}
static bool source_binary_check(SourceContext *ctx,AstNode *site,AstNodeType operation,
    XrXirType left,XrXirType right,XrHirBinary *output) {
    XrHirBinaryOperator logical;
    if(!source_binary_operator(operation,&logical))return source_fail(ctx,site,XR_XIR_BAD_STRUCTURE,"unknown arithmetic operator");
    XrXirSourceRange range=source_query_range(ctx,site,NULL);
    XrHirBinaryInput input={.origin={site?site->node_id:0,range.module,range.line,range.column,range.end_line,range.end_column},
        .operation=logical,.left=left,.right=right,
        .argument_bound=ctx->effect_argument && ctx->effect_argument->owner==ctx->function && ctx->effect_argument->syntax==site};
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    XrHirConversionContext context={&ctx->compile,&ctx->types,&proof,ctx->request,ctx,source_conversion_intern};
    const char *reason=NULL;XrXirStatus status=xr_hir_binary_check(&context,&input,output,&reason);
    return status==XR_XIR_OK || source_fail(ctx,site,status,reason);
}
static bool source_binary_emit(SourceContext *ctx,const XrHirBinary *node,
    SourceValue left,SourceValue right,SourceValue *value) {
    return source_conversion_emit(ctx,&node->left,&left) && source_conversion_emit(ctx,&node->right,&right) &&
        source_recipe_record(ctx,xr_hir_binary_instruction(node,left.id,right.id),value);
}
static bool source_binary_apply(SourceContext *ctx,AstNode *node,AstNodeType operation,
    SourceValue left,SourceValue right,SourceValue *value) {
    XrHirBinary typed;
    return source_binary_check(ctx,node,operation,left.type,right.type,&typed) && source_binary_emit(ctx,&typed,left,right,value);
}
