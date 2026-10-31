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
static bool source_copy_range_type(SourceContext *ctx,AstNode *node,XrXirType array,XrXirType *view) {
    XrXirType element=xr_xir_array_element(&ctx->types,array);
    if (!xr_xir_type_is_array(&ctx->types,array) || (element!=XR_XIR_I64 && element!=XR_XIR_U8))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy range requires an actual Array<i64> or Array<u8> position");
    return source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=element},view);
}
static bool source_copy_range(SourceContext *ctx,AstNode *node,SourceValue *value) {
    AstNode *slice=node->as.unary.operand,*root=slice->as.slice_expr.source;
    while (root && root->type==AST_GROUPING) {
        if (!source_request_poll(ctx,node)) return false;
        root=root->as.grouping;
    }
    if (!root || root->type!=AST_VARIABLE)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy range requires a stable named Array position");
    SourceName *binding=visible_name(ctx,root->as.variable.name);
    if (!binding || (binding->kind!=SOURCE_LOCAL && binding->kind!=SOURCE_SLOT))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy range requires a stable typed Array binding");
    if (binding->kind==SOURCE_SLOT && binding->module!=ctx->bodies[ctx->function].module)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"copy range requires a receiver-local Array owner");
    XrXirType owned=binding->type,view_type,cell;SourceValue snapshot,length,raw[2],args[3],view={0};
    /* Retain the selected logical value before bounds may rebind the caller's
     * root. The private Cell owns this snapshot, not a borrowed caller handle. */
    if (!source_copy_range_type(ctx,node,owned,&view_type) || !expression(ctx,root,&snapshot) ||
        !source_slice_raw_bounds(ctx,slice,raw) || !source_cell_type(ctx,owned,&cell) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{snapshot.id},{0},0,{0}},&length) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{snapshot.id},{0},0,{0}},args) ||
        !source_slice_bounds(ctx,slice,length,raw,args+1) ||
        !source_recipe_group(ctx,(XrXirInstruction){XR_XIR_VIEW_NEW,view_type,{0},{0},0,{0}},args,3,&view) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_VIEW_COPY,owned,{view.id},{0},0,{0}},value)) return false;
    /* Materialization owns its elements before ending this temporary loan. */
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_VIEW_END,XR_XIR_UNIT,{view.id},{0},0,{0}},NULL);
}
static bool source_copy(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    if (node->as.unary.operand && node->as.unary.operand->type==AST_SLICE_EXPR)
        return source_copy_range(ctx,node,value);
    AstNode *position=node->as.unary.operand;SourceValue input;
    if(!source_copy_position(ctx,position) || !source_plan_expression(ctx,position,expected,&input) ||
        !source_copy_type(ctx,node,input.type))return false;
    if(input.type==XR_XIR_UNIT){*value=input;return true;}
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_COPY,input.type,{input.id},{0},0,{0}},value);
}
