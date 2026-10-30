/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_index.inc.c - Exact Array and Map indexing with one receiver
 */
static bool source_index_map_reference(SourceContext *ctx, AstNode *node, XrXirType type) {
    const XrNativeTypeDeclaration *native = source_hash_native(ctx, type);
    uint32_t declaration = 0;
    return native && source_native_hash_declaration(ctx, native, &declaration) &&
        source_query_target_token_reference(ctx, node, declaration, XR_XIR_SOURCE_TYPE_USE);
}
static bool source_index_get(SourceContext *ctx, AstNode *node, SourceValue *value) {
    AstNode *receiver = node->as.index_get.array, *index = node->as.index_get.index;
    SourceValue container = {0}, key = {0};
    if (!source_array_receiver(ctx, receiver, index, false, &container)) return false;
    if (source_range_is(ctx,container.type)) return source_range_index(ctx,node,container,index,value);
    uint32_t view_kind=0;XrXirType view_element=XR_XIR_UNIT;
    if (source_view_shape(ctx,container.type,&view_kind,&view_element)) {
        if (!source_plan_expression(ctx,index,(SourceExpectedType){true,XR_XIR_I64,false,false},&key)) return false;
        if (key.type!=XR_XIR_I64) return source_fail(ctx,index,XR_XIR_BAD_TYPE,"view index requires exact i64");
        return source_recipe_record(ctx,(XrXirInstruction){XR_XIR_VIEW_GET,view_element,
            {container.id,key.id},{0},0,{0}},value);
    }
    if (!xr_xir_type_is_map(&ctx->types, container.type))
        return source_array_get(ctx, node, receiver, index, &container, NULL, value);
    XrHirPlaceAction shape,action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_SHAPE,.parent=container.place,
        .type=container.type,.base=container.id},&shape)||
        !source_hir_place_emitted(ctx,&shape,NULL,ctx->bodies[ctx->function].count)||
        !source_plan_expression(ctx,index,(SourceExpectedType){true,shape.key,false,false},&key)||
        !source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_INDEX_READ,.parent=container.place,
            .type=container.type,.base=container.id,.key=key.type,.selector=key.id,.failure="Map index requires its exact key type"},&action))return false;
    uint32_t before=ctx->bodies[ctx->function].count;
    return source_recipe_record(ctx,(XrXirInstruction){action.operation,action.type,{container.id,key.id},{0},0,{0}},value)&&
        source_hir_place_emitted(ctx,&action,value,before)&&source_index_map_reference(ctx,node,container.type);
}
static bool source_index_set(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceValue args[3] = {{0}};
    if (!source_array_receiver(ctx, node->as.index_set.array, NULL, true, args)) return false;
    XrHirPlaceAction shape,action;
    if(!source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_SHAPE,.parent=args[0].place,
        .type=args[0].type,.failure="indexed writable receiver is not an Array or Map"},&shape)||
        !source_hir_place_emitted(ctx,&shape,NULL,ctx->bodies[ctx->function].count)||
        !source_plan_expression(ctx,node->as.index_set.index,(SourceExpectedType){true,shape.key,false,false},args+1)||
        !source_plan_expression(ctx,node->as.index_set.value,(SourceExpectedType){true,shape.type,false,false},args+2)||
        !source_hir_place_check(ctx,node,(XrHirPlaceInput){.kind=XR_HIR_PLACE_INDEX_WRITE,.parent=args[0].place,
            .key=args[1].type,.selector=args[1].id,.value=args[2].type,.value_id=args[2].id,
            .failure="indexed assignment requires exact key and element types"},&action))return false;
    if(action.operation==XR_XIR_ARRAY_SET)return source_array_set_values(ctx,node,args,&action,value);
    uint32_t before=ctx->bodies[ctx->function].count;
    if(!source_recipe_group(ctx,(XrXirInstruction){action.operation,action.type,{0},{0},0,{0}},args,3,NULL)||
        !source_hir_place_emitted(ctx,&action,NULL,before)||!source_index_map_reference(ctx,node,args[0].type))return false;
    *value=args[2];return true;
}
