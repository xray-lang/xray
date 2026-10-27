/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_view.inc.c - Borrowed range expressions with explicit result types
 *
 * KEY CONCEPT:
 *   A source root remains a real Cell operand; a view never becomes an Array.
 */
/* The fixed lowered declaration grants only this real readonly view recipe.
 * A writable binding keeps its actual Cell root; other immutable String values
 * are retained in a local Cell whose lifetime is checked and normalized. */
static bool source_string_bytes_call(SourceContext *ctx,AstNode *node,SourceValue receiver,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const CallExprNode *call=&node->as.call_expr;
    if (!native || native->id!=2 || !member || member!=xr_native_declaration_member(native,"bytes") ||
        member->id!=18 || !member->is_method || !member->is_public || member->is_static ||
        !member->lowered || member->receiver!=XR_NATIVE_RECEIVER_READ || member->parameter_count ||
        member->operation!=XR_NATIVE_OPERATION_NONE || member->result!=XR_NATIVE_TERM_UNADMITTED ||
        !source_text_same(ctx,node,member->signature,"() -> Slice<u8>") ||
        !source_text_same(ctx,node,member->result_text,"Slice < u8 >") ||
        receiver.type!=XR_XIR_STRING || call->arg_count || call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"bytes requires its exact lowered readonly String declaration");
    XrXirType result,cell;
    if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},&result) ||
        !source_string_value_reference(ctx,call->callee,member,result)) return false;
    AstNode *syntax=call->callee->as.member_access.object;
    while (syntax && syntax->type==AST_GROUPING) syntax=syntax->as.grouping;
    SourceName *binding=syntax && syntax->type==AST_VARIABLE ? visible_name(ctx,syntax->as.variable.name) : NULL;
    SourceValue arguments[3]={{0}};
    if (binding && (binding->kind==SOURCE_LOCAL || binding->kind==SOURCE_SLOT) &&
        binding->mutable && binding->type==XR_XIR_STRING) {
        if (!source_binding_cell(ctx,binding,arguments)) return false;
    } else if (!source_cell_type(ctx,XR_XIR_STRING,&cell) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{receiver.id},{0},0,{0}},arguments)) return false;
    for (uint32_t i=1;i<3;++i)
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},arguments+i)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_VIEW_NEW,result,{0},{0},1,{0}},arguments,3,value);
}
static bool source_slice_root(SourceContext *ctx,AstNode *node,SourceValue *root,bool *reborrow) {
    while (node && node->type==AST_GROUPING) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        node=node->as.grouping;
    }
    if (!node || node->type!=AST_VARIABLE)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"projected view root has not completed Source admission");
    SourceName *binding=visible_name(ctx,node->as.variable.name);
    if (!binding || (binding->kind!=SOURCE_LOCAL && binding->kind!=SOURCE_SLOT))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"borrowed range requires a stable typed binding");
    uint32_t kind=0;XrXirType element=XR_XIR_UNIT;
    *reborrow=source_view_shape(ctx,binding->type,&kind,&element);
    if (*reborrow) return expression(ctx,node,root);
    if (!xr_xir_type_is_array(&ctx->types,binding->type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"slice operator requires an Array or borrowed view");
    if (binding->mutable)
        return source_query_reference(ctx,node,binding,binding,XR_XIR_SOURCE_READ) && source_binding_cell(ctx,binding,root);
    SourceValue value={0};XrXirType cell;
    return expression(ctx,node,&value) && source_cell_type(ctx,value.type,&cell) &&
        source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{value.id},{0},0,{0}},root);
}
static bool source_slice(SourceContext *ctx,AstNode *node,SourceExpectedType expected,SourceValue *value) {
    XrXirType target=expected.type;
    if (expected.present && xr_xir_type_is_nullable(&ctx->types,target)) target=xr_xir_nullable_element(&ctx->types,target);
    uint32_t kind=0;XrXirType element=XR_XIR_UNIT;
    if (!expected.present || !source_view_shape(ctx,target,&kind,&element))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"slice expression requires an explicit Slice or MutSlice target type");
    AstNode *syntax=node->as.slice_expr.source;SourceValue args[3]={{0}};bool reborrow=false;
    if (!source_slice_root(ctx,syntax,args,&reborrow)) return false;
    XrXirType source_element=XR_XIR_UNIT;uint32_t parent_kind=0;
    if (reborrow) {
        if (!source_view_shape(ctx,args[0].type,&parent_kind,&source_element) ||
            (kind==XR_XIR_TYPE_MUT_SLICE && parent_kind!=XR_XIR_TYPE_MUT_SLICE))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"a shared view cannot grant a mutable subrange");
    } else {
        XrXirType array=xr_xir_cell_element(&ctx->types,args[0].type);
        source_element=xr_xir_array_element(&ctx->types,array);
        while (syntax && syntax->type==AST_GROUPING) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            syntax=syntax->as.grouping;
        }
        SourceName *binding=syntax ? visible_name(ctx,syntax->as.variable.name) : NULL;
        if (kind==XR_XIR_TYPE_MUT_SLICE && (!binding || !binding->mutable))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"mutable view requires an exclusively writable owner");
    }
    if (source_element!=element)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"view element differs from its exact root element");
    AstNode *bounds[2]={node->as.slice_expr.start,node->as.slice_expr.end};
    for (uint32_t i=0;i<2;++i) {
        if (bounds[i]) {
            if (!source_plan_expression(ctx,bounds[i],(SourceExpectedType){true,XR_XIR_I64,false,false},args+i+1)) return false;
            if (args[i+1].type!=XR_XIR_I64) return source_fail(ctx,bounds[i],XR_XIR_BAD_TYPE,"view bounds require exact i64 values");
        } else if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},args+i+1)) return false;
    }
    return source_recipe_group(ctx,(XrXirInstruction){reborrow ? XR_XIR_VIEW_REBORROW : XR_XIR_VIEW_NEW,
        target,{0},{0},bounds[1] ? 0 : 1,{0}},args,3,value);
}
