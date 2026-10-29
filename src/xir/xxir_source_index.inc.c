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
    const XrXirTypeNode *map = xr_xir_type_node(&ctx->types, container.type);
    XrXirType key_type = map->element, element = map->result;
    if (!source_plan_expression(ctx, index, (SourceExpectedType){true, key_type, false, false}, &key)) return false;
    if (key.type != key_type)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Map index requires its exact key type");
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_MAP_INDEX, element,
        {container.id, key.id}, {0}, 0, {0}}, value) && source_index_map_reference(ctx, node, container.type);
}
static bool source_index_set(SourceContext *ctx, AstNode *node, SourceValue *value) {
    SourceValue args[3] = {{0}};
    if (!source_array_receiver(ctx, node->as.index_set.array, NULL, true, args)) return false;
    bool map = xr_xir_type_is_map(&ctx->types, args[0].type);
    if (!map && !xr_xir_type_is_array(&ctx->types, args[0].type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indexed writable receiver is not an Array or Map");
    const XrXirTypeNode *container = xr_xir_type_node(&ctx->types, args[0].type);
    XrXirType key = map ? container->element : XR_XIR_I64;
    XrXirType element = map ? container->result : container->element;
    if (!source_plan_expression(ctx, node->as.index_set.index, (SourceExpectedType){true, key, false, false}, args + 1) ||
        !source_plan_expression(ctx, node->as.index_set.value, (SourceExpectedType){true, element, false, false}, args + 2)) return false;
    if (args[1].type != key || args[2].type != element)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "indexed assignment requires exact key and element types");
    if (!map) return source_array_set_values(ctx, node, args, NULL, true, value);
    if (!source_recipe_group(ctx, (XrXirInstruction){XR_XIR_MAP_SET, XR_XIR_UNIT, {0}, {0}, 0, {0}}, args, 3, value) ||
        !source_index_map_reference(ctx, node, args[0].type)) return false;
    *value = args[2];
    return true;
}
