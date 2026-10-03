/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_enum_text.inc.c - Declaration-bound enum identity strings
 *
 * KEY CONCEPT:
 *   Tag-only selection never reads payloads or grants construction authority.
 */
static bool source_enum_text_literal(SourceContext *ctx, AstNode *node,
    const XrXirNominalDeclaration *declaration, uint32_t variant, bool qualified, SourceValue *value) {
    const XrXirLiteral *name = &declaration->variants[variant].name;
    uint64_t length = name->length;
    if (qualified) length += (uint64_t)declaration->name.length + 1;
    if (length > UINT32_MAX || length >= SIZE_MAX)
        return source_fail(ctx, node, XR_XIR_BUDGET, "enum identity string budget exhausted");

    char *bytes = source_alloc(ctx, (size_t)length + 1, 1);
    if (!bytes) return false;
    size_t prefix = 0;
    if (qualified) {
        if (!source_copy_bytes(ctx,node,bytes,declaration->name.bytes,declaration->name.length) ||
            !source_work(ctx,node)) return false;
        prefix = declaration->name.length; bytes[prefix++] = '.';
    }
    if (!source_copy_bytes(ctx,node,bytes+prefix,name->bytes,name->length)) return false;
    return source_string_literal(ctx, node, bytes, (size_t)length, value);
}
static bool source_enum_text(SourceContext *ctx, AstNode *node, SourceValue receiver,
    bool qualified, SourceValue *value) {
    const XrXirTypeNode *type = xr_xir_type_node(&ctx->types, receiver.type);
    if (!type || !xr_xir_type_is_enum(&ctx->types, receiver.type))
        return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum identity requires a declared enum type");
    uint32_t owner = type->nominal.declaration;
    const XrXirNominalDeclaration *declaration = &ctx->nominals.declarations[owner];
    uint32_t count = declaration->variant_count;
    if (!count) return source_fail(ctx, node, XR_XIR_BAD_TYPE, "enum identity requires a nonempty enum");
    uint32_t member = ctx->nominal_variants[owner][count + (qualified ? 2 : 1)];
    if (!source_query_target_reference(ctx, source_query_range(ctx, node, NULL), member,
        qualified ? XR_XIR_SOURCE_CALL : XR_XIR_SOURCE_READ)) return false;
    SourceValue tag;
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ENUM_TAG, XR_XIR_I64, {receiver.id,0}, {0}, 0, {0}}, &tag) ||
        !source_enum_text_literal(ctx, node, declaration, count - 1, qualified, value)) return false;
    for (uint32_t variant = 0; variant + 1 < count; ++variant) {
        SourceValue ordinal, condition, selected;
        if (!source_work(ctx, node) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, variant, {0}}, &ordinal) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_EQ_INT, XR_XIR_BOOL, {tag.id,ordinal.id}, {0}, 0, {0}}, &condition)) return false;
        SourceFunction *body = &ctx->bodies[ctx->function];
        uint32_t branch = body->count, yes = body->block_count;
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id}, {0}, 0, {0}}, NULL) ||
            !begin_block(ctx) || !source_enum_text_literal(ctx, node, declaration, variant, qualified, &selected)) return false;
        uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
        uint32_t no = body->block_count;
        if (!begin_block(ctx)) return false;
        uint32_t no_jump = body->count;
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_JUMP, XR_XIR_UNIT, {0}, {0}, 0, {0}}, NULL)) return false;
        uint32_t join = body->block_count;
        if (!begin_block(ctx)) return false;
        body->recipes[branch].instruction.targets[0] = yes; body->recipes[branch].instruction.targets[1] = no;
        body->recipes[yes_jump].instruction.targets[0] = join; body->recipes[no_jump].instruction.targets[0] = join;
        SourceValue inputs[] = {{yes_end,XR_XIR_UNIT},selected,{no,XR_XIR_UNIT},*value};
        if (!source_recipe_group(ctx, (XrXirInstruction){XR_XIR_PHI, XR_XIR_STRING, {0}, {0}, 0, {0}}, inputs, 4, value)) return false;
    }
    return true;
}
