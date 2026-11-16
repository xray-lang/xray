/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_set_construct.inc.c - Set construction from one Array snapshot
 *
 * KEY CONCEPT:
 *   The ordinary Array expression owns evaluation and element inference. The
 *   constructor inserts its snapshot into a fresh Set through existing places.
 */
static bool source_set_construct(SourceContext *ctx, AstNode *node,
    SourceExpectedType expected, SourceValue *value) {
    SourceCallSyntax call;
    if (!source_call_syntax(ctx, node, &call)) return false;
    if (call.arg_count > 1 || call.type_arg_count > 1 || call.arg_labels ||
        (call.arg_count && call.arg_accesses && call.arg_accesses[0] != XR_CALL_ARG_PLAIN))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Set construction takes one ordinary Array argument");
    XrXirType type = XR_XIR_UNIT, element = XR_XIR_UNIT;
    if (expected.present && xr_xir_type_is_nullable(&ctx->types, expected.type))
        expected.type = xr_xir_nullable_element(&ctx->types, expected.type);
    if (call.type_arg_count) {
        XrTypeRef reference = {.kind=XR_TREF_GENERIC, .name="Set", .children=call.type_args,
            .nchildren=1, .line=node->line, .column=node->column};
        if (!source_native_hash_type(ctx, &reference, &type)) return false;
        element = xr_xir_type_node(&ctx->types, type)->element;
    } else if (expected.present && xr_xir_type_is_set(&ctx->types, expected.type)) {
        type = expected.type;
        element = xr_xir_type_node(&ctx->types, type)->element;
    }
    SourceValue array = {0};
    if (call.arg_count) {
        SourceExpectedType context = {0};
        if (element != XR_XIR_UNIT) {
            if (!source_array_element_type(ctx, element, &context.type)) return false;
            context.present = true;
            context.inferred = expected.inferred && !call.type_arg_count;
        }
        if (!source_collection_expression(ctx, call.arguments[0], context, context.inferred, &array)) return false;
        if (!xr_xir_type_is_array(&ctx->types, array.type))
            return source_fail(ctx, call.arguments[0], XR_XIR_BAD_TYPE, "Set construction requires an Array value");
        XrXirType actual = xr_xir_array_element(&ctx->types, array.type);
        if (element != XR_XIR_UNIT && actual != element) {
            if (!context.inferred)
                return source_fail(ctx, call.arguments[0], XR_XIR_BAD_TYPE, "Set Array element type differs from its declared element type");
            type = XR_XIR_UNIT;
        }
        element = actual;
    }
    if (element == XR_XIR_UNIT || xr_xir_type_is_cell(&ctx->types, element))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "empty Set construction requires an element type or Set context");
    uint32_t declaration = 0;
    const XrNativeTypeDeclaration *native = xr_native_declaration_by_id(XR_NATIVE_DECLARATION_SET);
    if (!source_native_hash_declaration(ctx, native, &declaration) ||
        (type == XR_XIR_UNIT && !source_intern_type(ctx,
            (XrXirTypeNode){.kind=XR_XIR_TYPE_SET, .element=element}, &type))) return false;
    if (!source_query_target_token_reference(ctx, node, declaration, XR_XIR_SOURCE_TYPE_USE)) return false;
    SourceValue seed;
    if (!source_recipe_group(ctx, (XrXirInstruction){XR_XIR_SET_NEW,type,{0},{0},0,{0}}, NULL, 0, &seed))
        return false;
    if (!call.arg_count) { *value = seed; return true; }
    XrXirType cell;
    SourceValue store, place, length, zero, entry;
    if (!source_cell_type(ctx, type, &cell) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_NEW,cell,{seed.id},{0},0,{0}}, &store) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_PLACE,type,{store.id},{0},0,{0}}, &place) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ARRAY_LEN,XR_XIR_I64,{array.id},{0},0,{0}}, &length) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},0,{0}}, &zero)) return false;
    SourceCounter counter = {0};
    if (!source_counter_open(ctx, zero, length, false, &counter) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ARRAY_GET,element,{array.id,counter.index.id},{0},0,{0}}, &entry) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_SET_ADD,XR_XIR_UNIT,{place.id,entry.id},{0},0,{0}}, NULL) ||
        !source_counter_close(ctx, &counter)) return false;
    return source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CELL_READ,type,{store.id},{0},0,{0}}, value);
}
