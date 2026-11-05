/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_string_bytes.inc.c - Borrowed UTF-8 calls rooted in real bindings
 *
 * KEY CONCEPT:
 *   Bind the original Cell before evaluating a receiver as an owned snapshot.
 */
static bool source_string_bytes_contract(SourceContext *ctx,AstNode *node,
    const XrNativeIntrinsicDeclaration **intrinsic,XrXirType *result) {
    const CallExprNode *call=&node->as.call_expr;
    if (!source_string_declaration(ctx)) return false;
    const XrNativeTypeDeclaration *native=xr_native_declaration_by_id(XR_NATIVE_DECLARATION_STRING);
    const XrNativeMemberDeclaration *member=source_native_member(ctx,native,call->callee->as.member_access.name);
    const XrNativeIntrinsicDeclaration *selected=member?xr_native_intrinsic_for_member(native,member):NULL;
    if (!selected || xr_xir_intrinsic(selected->id)!=selected || !xr_xir_intrinsic_is_string_bytes(selected) ||
        !member->is_public || call->arg_count || call->type_arg_count || call->default_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"string bytes requires its canonical zero-argument declaration");
    if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},result)) return false;
    *intrinsic=selected;return true;
}
static bool source_string_bytes_call(SourceContext *ctx,AstNode *node,SourceValue *value) {
    const XrNativeIntrinsicDeclaration *intrinsic=NULL;XrXirType result;
    if (!source_string_bytes_contract(ctx,node,&intrinsic,&result)) return false;
    AstNode *syntax=node->as.call_expr.callee->as.member_access.object,*root=syntax;
    while (root && root->type==AST_GROUPING) {
        if (!source_work(ctx,root)) return false;
        root=root->as.grouping;
    }
    SourceName *binding=root && root->type==AST_VARIABLE?visible_name(ctx,root->as.variable.name):NULL;
    if (!binding || binding->kind!=SOURCE_LOCAL || binding->type!=XR_XIR_STRING || binding->construction)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"string byte view root has not completed Source admission");
    SourceName saved=*binding;SourceValue cell={0};
    if (saved.mutable) {
        if (!source_query_reference(ctx,root,binding,binding,XR_XIR_SOURCE_READ) ||
            !source_binding_cell(ctx,&saved,&cell)) return false;
    } else {
        SourceValue input={0};XrXirType physical;
        if (!expression(ctx,root,&input) || input.type!=XR_XIR_STRING ||
            !source_cell_type(ctx,XR_XIR_STRING,&physical) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CELL_NEW,physical,{input.id},{0},0,{0}},&cell))
            return false;
    }
    if (!source_query_expression(ctx,syntax,XR_XIR_STRING) ||
        !source_string_value_reference(ctx,node->as.call_expr.callee,intrinsic->member,result)) return false;
    return source_recipe_group(ctx,(XrXirInstruction){XR_XIR_CALL_INTRINSIC,result,{0},{0},intrinsic->id,{0}},
        &cell,1,value);
}
