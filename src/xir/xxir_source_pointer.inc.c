/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pointer.inc.c - Typed byte-pointer operations at explicit unsafe sites
 *
 * KEY CONCEPT:
 *   Pointer access checks are independent of binding mutability. A readonly
 *   pointer cannot acquire write access from a variable, ref or unsafe block.
 */
static bool source_pointer_access(SourceContext *ctx,AstNode *node,XrXirType type,bool write) {
    if (!xr_xir_type_is_pointer(&ctx->types,type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"raw access requires an actual byte pointer");
    if (write && !xr_xir_pointer_writable(&ctx->types,type))
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"readonly pointer cannot write memory");
    if (!ctx->bodies[ctx->function].unsafe_depth)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"raw pointer access requires unsafe");
    return true;
}
static bool source_pointer_namespace(SourceContext *ctx,AstNode *node) {
    return node && node->type==AST_NEW_EXPR && node->as.new_expr.is_type_namespace &&
        !node->as.new_expr.module_name && node->as.new_expr.class_name &&
        (source_text_same(ctx,node,node->as.new_expr.class_name,"Ptr") ||
         source_text_same(ctx,node,node->as.new_expr.class_name,"MutPtr")) &&
        !source_native_type_shadowed(ctx,node->as.new_expr.class_name);
}
static bool source_pointer_namespace_type(SourceContext *ctx,AstNode *node,XrXirType *type) {
    NewExprNode *space=&node->as.new_expr;XrXirType element=XR_XIR_UNIT;
    if (space->arg_count || space->type_arg_count!=1 || !space->type_args)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pointer namespace requires one element type");
    if (!source_type(ctx,space->type_args[0],&element)) return false;
    if (element!=XR_XIR_U8)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"only byte pointer elements are currently executable");
    return source_intern_type(ctx,(XrXirTypeNode){.kind=source_text_same(ctx,node,space->class_name,"Ptr")?
        XR_XIR_TYPE_PTR:XR_XIR_TYPE_MUT_PTR,.element=element},type);
}
static bool source_pointer_null_call(SourceContext *ctx,AstNode *node,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;MemberAccessNode *member=&call->callee->as.member_access;
    if (!source_text_same(ctx,node,member->name,"null") || call->arg_count || call->type_arg_count)
        return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pointer static namespace only provides null()");
    XrXirType type=XR_XIR_UNIT;
    return source_pointer_namespace_type(ctx,member->object,&type) &&
        source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_PTR_NULL,.type=type},value);
}
static bool source_pointer_call(SourceContext *ctx,AstNode *node,SourceValue receiver,SourceValue *value) {
    CallExprNode *call=&node->as.call_expr;const char *name=call->callee->as.member_access.name;
    if (call->type_arg_count) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pointer member has no type parameters");
    if (source_text_same(ctx,node,name,"offset")) {
        XrXirType parameter=XR_XIR_I64;SourceValue argument={0};uint32_t count=0;
        return source_native_read_arguments(ctx,node,&parameter,1,1,&argument,&count,
            "pointer offset requires one exact i64") && source_recipe_record(ctx,
            (XrXirInstruction){.op=XR_XIR_PTR_OFFSET,.type=receiver.type,.args={receiver.id,argument.id}},value);
    }
    if (source_text_same(ctx,node,name,"deref")) {
        if (call->arg_count) return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pointer deref accepts no arguments");
        if (!source_pointer_access(ctx,node,receiver.type,false)) return false;
        SourceValue zero={0};
        return source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},&zero) &&
            source_recipe_record(ctx,(XrXirInstruction){.op=XR_XIR_PTR_LOAD,.type=XR_XIR_U8,.args={receiver.id,zero.id}},value);
    }
    if (source_text_same(ctx,node,name,"copyFromNonOverlapping")) {
        if (!source_pointer_access(ctx,node,receiver.type,true)) return false;
        XrXirType read=XR_XIR_UNIT;
        if (!source_intern_type(ctx,(XrXirTypeNode){.kind=XR_XIR_TYPE_PTR,.element=XR_XIR_U8},&read)) return false;
        XrXirType parameters[]={read,XR_XIR_I64};SourceValue args[3]={receiver,{0},{0}};uint32_t count=0;
        return source_native_read_arguments(ctx,node,parameters,2,2,args+1,&count,
            "pointer copy requires a readonly byte pointer and exact i64 count") &&
            source_recipe_group(ctx,(XrXirInstruction){.op=XR_XIR_PTR_COPY},args,3,value);
    }
    return source_fail(ctx,node,XR_XIR_BAD_TYPE,"pointer member is not executable");
}
