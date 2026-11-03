/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_byte_array.inc.c - Governed byte receiver recipes
 *
 * KEY CONCEPT:
 *   The result owns its candidate before the single checked destination publication.
 */
static bool source_byte_array_call(SourceContext *ctx,AstNode *node,
    const XrNativeMemberDeclaration *member,SourceValue *value) {
    bool repeat=member->operation==XR_NATIVE_OPERATION_ARRAY_BYTE_REPEAT;
    uint32_t count=repeat?2u:1u,supplied=0;
    SourceValue root,args[2]={{0}},current,candidate;
    XrXirType parameters[2]={XR_XIR_I64,XR_XIR_I64},view_type=XR_XIR_UNIT;
    if (!source_value_place(ctx,node->as.call_expr.callee->as.member_access.object,&root)) return false;
    if (!xr_xir_type_is_array(&ctx->types,root.type) || xr_xir_array_element(&ctx->types,root.type)!=XR_XIR_U8)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"byte bulk methods require an exact writable Array<u8>");
    if (!repeat) {
        if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_SLICE,.element=XR_XIR_U8},&view_type))return false;
        parameters[0]=view_type;
    }
    if (!source_native_read_arguments(ctx,node,parameters,count,count,args,&supplied,
        "byte bulk method requires its exact declared arguments"))return false;
    if (!repeat) {
        SourceValue zero,borrow[3],temporary,snapshot;
        if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}},&zero))return false;
        borrow[0]=args[0];borrow[1]=borrow[2]=zero;
        if (!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_VIEW_REBORROW,view_type,{0},{0},1,{0}},borrow,3,&temporary) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_VIEW_COPY,root.type,{temporary.id},{0},0,{0}},&snapshot) ||
            !source_recipe_record(ctx,(XrXirInstruction){XR_XIR_VIEW_END,XR_XIR_UNIT,{temporary.id},{0},0,{0}},NULL))return false;
        args[0]=snapshot;
    }
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_PLACE_READ,root.type,{root.id},{0},0,{0}},&current))return false;
    if (repeat) {
        SourceValue inputs[]={current,args[0],args[1]};
        if (!source_recipe_group(ctx,(XrXirInstruction){XR_XIR_BYTE_ARRAY_REPEAT,root.type,{0},{0},0,{0}},inputs,3,&candidate))return false;
    } else if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_BYTE_ARRAY_APPEND,root.type,
        {current.id,args[0].id},{0},0,{0}},&candidate))return false;
    if (!source_recipe_record(ctx,(XrXirInstruction){XR_XIR_PLACE_WRITE,XR_XIR_UNIT,{root.id,candidate.id},{0},0,{0}},NULL) ||
        !source_array_member_reference(ctx,node,member) ||
        !source_array_call_signature(ctx,node,&(SourceNativeCallShape){root.type,parameters,count},candidate.type))return false;
    *value=candidate;return true;
}
