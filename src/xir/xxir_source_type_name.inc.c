/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_type_name.inc.c - typeName(value) over proved static types
 *
 * KEY CONCEPT:
 *   The name of a proved type is a compile-time constant; only a Nullable operand needs a runtime
 *   choice between the element name and "null". Generic nominal names list their arguments.
 */
#define SOURCE_TYPE_NAME_LIMIT 256u
typedef struct SourceTypeName { char bytes[SOURCE_TYPE_NAME_LIMIT]; size_t length; bool overflow; } SourceTypeName;
static void source_type_name_append(SourceTypeName *name, const char *text, size_t length) {
    if (length > SOURCE_TYPE_NAME_LIMIT - name->length) { name->overflow = true; return; }
    for (size_t i = 0; i < length; ++i) name->bytes[name->length + i] = text[i];
    name->length += length;
}
static const char *source_scalar_type_name(XrXirType type) {
    switch (type) {
    case XR_XIR_UNIT: return TYPE_NAME_UNIT;
    case XR_XIR_BOOL: return TYPE_NAME_BOOL;
    case XR_XIR_RUNE: return TYPE_NAME_RUNE;
    case XR_XIR_I8: return TYPE_NAME_I8;
    case XR_XIR_I16: return TYPE_NAME_I16;
    case XR_XIR_I32: return TYPE_NAME_I32;
    case XR_XIR_I64: return TYPE_NAME_I64;
    case XR_XIR_U8: return TYPE_NAME_U8;
    case XR_XIR_U16: return TYPE_NAME_U16;
    case XR_XIR_U32: return TYPE_NAME_U32;
    case XR_XIR_U64: return TYPE_NAME_U64;
    case XR_XIR_F32: return TYPE_NAME_F32;
    case XR_XIR_F64: return TYPE_NAME_F64;
    case XR_XIR_STRING: return TYPE_NAME_STRING;
    case XR_XIR_PANIC_INFO: return TYPE_NAME_PANIC_INFO;
    default: return NULL;
    }
}
/* `top` is the operand's own type: an Array reports just "Array", while inside arguments it shows its element. */
static bool source_type_name_collect(SourceContext *ctx, AstNode *node, XrXirType type, bool top, uint32_t depth,
    SourceTypeName *name) {
    if (!source_work(ctx, node)) return false;
    if (depth > 32) return source_fail(ctx, node, XR_XIR_BUDGET, "type name nesting exhausted");
    if (type == XR_XIR_ERROR)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "Error type name is not admitted");
    if ((uint32_t) type >= XR_XIR_TYPE_PARAMETER_BASE && (uint32_t) type < XR_XIR_TYPE_PARAMETER_LIMIT)
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "generic type name is not admitted");
    const char *scalar = source_scalar_type_name(type);
    if (scalar) { source_type_name_append(name, scalar, strlen(scalar)); return true; }
    const XrXirTypeNode *shape = xr_xir_type_node(&ctx->types, type);
    if (!shape) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "typeName operand has no proved type");
    switch (shape->kind) {
    case XR_XIR_TYPE_ATOMIC:
        source_type_name_append(name, TYPE_NAME_ATOMIC, strlen(TYPE_NAME_ATOMIC));
        if (!top) {
            source_type_name_append(name, "<", 1);
            if (!source_type_name_collect(ctx, node, shape->element, false, depth + 1, name)) return false;
            source_type_name_append(name, ">", 1);
        }
        return true;
    case XR_XIR_TYPE_ARRAY:
        source_type_name_append(name, TYPE_NAME_ARRAY, strlen(TYPE_NAME_ARRAY));
        if (!top) {
            source_type_name_append(name, "<", 1);
            if (!source_type_name_collect(ctx, node, shape->element, false, depth + 1, name)) return false;
            source_type_name_append(name, ">", 1);
        }
        return true;
    case XR_XIR_TYPE_NULLABLE:
        if (!source_type_name_collect(ctx, node, shape->element, false, depth + 1, name)) return false;
        source_type_name_append(name, "?", 1);
        return true;
    case XR_XIR_TYPE_CALLABLE:
        source_type_name_append(name, TYPE_NAME_FUNCTION, strlen(TYPE_NAME_FUNCTION));
        return true;
    case XR_XIR_TYPE_NOMINAL: {
        const XrXirNominalDeclaration *declaration = &ctx->nominals.declarations[shape->nominal.declaration];
        source_type_name_append(name, (const char *) declaration->name.bytes, declaration->name.length);
        if (shape->nominal.argument_count) {
            source_type_name_append(name, "<", 1);
            for (uint32_t i = 0; i < shape->nominal.argument_count; ++i) {
                if (i) source_type_name_append(name, ", ", 2);
                if (!source_type_name_collect(ctx, node, shape->nominal.arguments[i], false, depth + 1, name)) return false;
            }
            source_type_name_append(name, ">", 1);
        }
        return true;
    }
    default: return source_fail(ctx, node, XR_XIR_BAD_TYPE, "typeName operand has no admitted type name");
    }
}
static bool source_type_name_literal(SourceContext *ctx, AstNode *node, XrXirType type, SourceValue *value) {
    SourceTypeName name = {{0}, 0, false};
    if (!source_type_name_collect(ctx, node, type, true, 0, &name)) return false;
    if (name.overflow) return source_fail(ctx, node, XR_XIR_BUDGET, "type name length exhausted");
    /* The literal pool keeps the bytes it is given, so they must outlive this stack frame. */
    char *bytes = source_alloc(ctx, name.length + 1, 1);
    if (!bytes || !source_copy_bytes(ctx, node, bytes, name.bytes, name.length)) return false;
    return source_string_literal(ctx, node, bytes, name.length, value);
}
static bool source_type_name(SourceContext *ctx, AstNode *node, SourceValue input, SourceValue *value) {
    if (!xr_xir_type_is_nullable(&ctx->types, input.type)) return source_type_name_literal(ctx, node, input.type, value);
    /* A Nullable operand reports the element's name when present and "null" otherwise. */
    SourceValue some;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_NULLABLE_IS_SOME, XR_XIR_BOOL, {input.id, 0}, {0}, 0, {0}}, &some))
        return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    if (!body->block_count && !begin_block(ctx)) return false;
    uint32_t branch = body->count, yes_block = body->block_count;
    SourceValue yes, no;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_BRANCH, XR_XIR_UNIT, {some.id}, {0}, 0, {0}}, NULL) ||
        !begin_block(ctx) || !source_type_name_literal(ctx, node, xr_xir_nullable_element(&ctx->types, input.type), &yes)) return false;
    uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx) || !source_string_literal(ctx, node, TYPE_NAME_NULL, strlen(TYPE_NAME_NULL), &no)) return false;
    uint32_t no_end = body->current_block.identity, no_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction) {XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0] = yes_block; body->recipes[branch].instruction.targets[1] = no_block;
    body->recipes[yes_jump].instruction.targets[0] = join; body->recipes[no_jump].instruction.targets[0] = join;
    SourceValue inputs[] = {{yes_end, XR_XIR_UNIT}, yes, {no_end, XR_XIR_UNIT}, no};
    return source_recipe_group(ctx, (XrXirInstruction) {XR_XIR_PHI, XR_XIR_STRING, {0}, {0}, 0, {0}}, inputs, 4, value);
}
