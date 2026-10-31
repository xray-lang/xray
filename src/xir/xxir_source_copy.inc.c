/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_copy.inc.c - Explicit logical copies of ordinary owned positions
 */
static bool source_copy_position(SourceContext *ctx,AstNode *node) {
    for(uint32_t depth=0;node && depth<128;++depth) {
        if(node->type==AST_VARIABLE || node->type==AST_THIS_EXPR)return true;
        if(node->type==AST_GROUPING)node=node->as.grouping;
        else if(node->type==AST_MEMBER_ACCESS)node=node->as.member_access.object;
        else if(node->type==AST_INDEX_GET)node=node->as.index_get.array;
        else return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy requires a named value position");
    }
    return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy position is missing or too deep");
}
static bool source_copy_type(SourceContext *ctx,AstNode *node,XrXirType type) {
    if(type==XR_XIR_UNIT)return true;
    uint32_t borrowed=0;
    XrXirStatus status=xr_xir_compile_view_type_flags(&ctx->compile,&ctx->types,type,&borrowed);
    if(status!=XR_XIR_OK)return source_fail(ctx,node,status,"copy operand type flags failed");
    if(borrowed || xr_xir_type_is_cell(&ctx->types,type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy of a borrowed value requires owned materialization");
    XrXirDeclarations declarations;XrXirModule module=source_module_view(ctx,&declarations);
    XrXirProofContext proof={&module,{XR_XIR_CONTEXT_FUNCTION,ctx->function,0}};
    status=xr_xir_compile_type_storage_prove(&ctx->compile,&proof,type);
    return status==XR_XIR_OK || source_fail(ctx,node,status,"copy requires an ordinary copyable value");
}
static bool source_copy(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    AstNode *position=node->as.unary.operand;SourceValue input;
    if(!source_copy_position(ctx,position) || !source_plan_expression(ctx,position,expected,&input) ||
        !source_copy_type(ctx,node,input.type))return false;
    if(input.type==XR_XIR_UNIT){*value=input;return true;}
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_COPY,input.type,{input.id},{0},0,{0}},value);
}
