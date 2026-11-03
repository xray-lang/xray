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
        member->operation!=XR_NATIVE_OPERATION_NONE || member->result!=XR_NATIVE_TERM_SLICE_U8 ||
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
/* Array source ranges normalize only after both explicit bounds were
 * evaluated. The owner length is already selected and never reread here. */
static bool source_slice_bound(SourceContext *ctx,SourceValue raw,SourceValue length,
    SourceValue zero,SourceValue *result) {
    XrXirType cell;SourceValue storage,negative,adjusted,below,above;SourceGuard guard;
    if (!source_cell_type(ctx,XR_XIR_I64,&cell) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{raw.id},{0},0,{0}},&storage) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{raw.id,zero.id},{0},0,{0}},&negative) ||
        !source_guard_open(ctx,negative,false,&guard) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ADD_INT,XR_XIR_I64,{length.id,raw.id},{0},0,{0}},&adjusted) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{storage.id,adjusted.id},{0},0,{0}},NULL) ||
        !source_guard_close(ctx,&guard) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{storage.id},{0},0,{0}},&adjusted) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{adjusted.id,zero.id},{0},0,{0}},&below) ||
        !source_guard_open(ctx,below,false,&guard) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{storage.id,zero.id},{0},0,{0}},NULL) ||
        !source_guard_close(ctx,&guard) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{storage.id},{0},0,{0}},&adjusted) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{length.id,adjusted.id},{0},0,{0}},&above) ||
        !source_guard_open(ctx,above,false,&guard) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{storage.id,length.id},{0},0,{0}},NULL) ||
        !source_guard_close(ctx,&guard)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{storage.id},{0},0,{0}},result);
}
static bool source_slice_raw_bounds(SourceContext *ctx,AstNode *node,SourceValue raw[2]) {
    AstNode *bounds[]={node->as.slice_expr.start,node->as.slice_expr.end};
    for (uint32_t i=0;i<2;++i) {
        if (bounds[i]) {
            if (!source_plan_expression(ctx,bounds[i],(SourceExpectedType){true,XR_XIR_I64,false,false},raw+i)) return false;
            if (raw[i].type!=XR_XIR_I64) return source_fail(ctx,bounds[i],XR_XIR_BAD_TYPE,"view bounds require exact i64 values");
        } else if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},raw+i)) return false;
    }
    return true;
}
static bool source_slice_bounds(SourceContext *ctx,AstNode *node,SourceValue length,
    const SourceValue raw[2],SourceValue output[2]) {
    AstNode *bounds[]={node->as.slice_expr.start,node->as.slice_expr.end};SourceValue zero;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero)) return false;
    for (uint32_t i=0;i<2;++i) {
        if (bounds[i]) {if (!source_slice_bound(ctx,raw[i],length,zero,output+i)) return false;}
        else output[i]=i?length:zero;
    }
    XrXirType cell;SourceValue start,backwards;SourceGuard guard;
    if (!source_cell_type(ctx,XR_XIR_I64,&cell) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,cell,{output[0].id},{0},0,{0}},&start) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_LT_INT,XR_XIR_BOOL,{output[1].id,output[0].id},{0},0,{0}},&backwards) ||
        !source_guard_open(ctx,backwards,false,&guard) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_WRITE,XR_XIR_UNIT,{start.id,output[1].id},{0},0,{0}},NULL) ||
        !source_guard_close(ctx,&guard)) return false;
    return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,XR_XIR_I64,{start.id},{0},0,{0}},output);
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
        SourceName *binding=syntax && syntax->type==AST_VARIABLE ? visible_name(ctx,syntax->as.variable.name) : NULL;
        if (kind==XR_XIR_TYPE_MUT_SLICE && (!binding || !binding->mutable))
            return source_fail(ctx,node,XR_XIR_BAD_TYPE,"mutable view requires an exclusively writable owner");
    }
    if (source_element!=element)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"view element differs from its exact root element");
    /* Borrowing selects the real Cell position once. Explicit bounds finish
     * before reading that same Cell's current payload and establishing a loan. */
    SourceValue raw[2],length,current;
    if (!source_slice_raw_bounds(ctx,node,raw)) return false;
    if (reborrow) {
        args[1]=raw[0];args[2]=raw[1];
        return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_VIEW_REBORROW,target,{0},{0},
            node->as.slice_expr.end?0:1,{0}},args,3,value);
    }
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_READ,
        xr_xir_cell_element(&ctx->types,args[0].type),{args[0].id},{0},0,{0}},&current) ||
        !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{current.id},{0},0,{0}},&length)) return false;
    return source_slice_bounds(ctx,node,length,raw,args+1) &&
        source_recipe_group(ctx,(XrXirInstruction){reborrow?XR_XIR_VIEW_REBORROW:XR_XIR_VIEW_NEW,
            target,{0},{0},0,{0}},args,3,value);
}
