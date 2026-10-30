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
    const XrXirTypeDescription *description, uint32_t variant, bool qualified, SourceValue *value) {
    const XrXirLiteral *name = &description->variants[variant].name;
    uint64_t length = name->length;
    if (qualified) length += (uint64_t)description->name.length + 1;
    if (length > UINT32_MAX || length >= SIZE_MAX)
        return source_fail(ctx, node, XR_XIR_BUDGET, "enum identity string budget exhausted");

    char *bytes = source_alloc(ctx, (size_t)length + 1, 1);
    if (!bytes) return false;
    size_t prefix = 0;
    if (qualified) {
        if (!source_copy_bytes(ctx,node,bytes,description->name.bytes,description->name.length) ||
            !((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        prefix = description->name.length; bytes[prefix++] = '.';
    }
    if (!source_copy_bytes(ctx,node,bytes+prefix,name->bytes,name->length)) return false;
    return source_string_literal(ctx, node, bytes, (size_t)length, value);
}
static bool source_enum_text(SourceContext *ctx, AstNode *node, SourceValue receiver,
    bool qualified, SourceValue *value) {
    XrXirTypeDescription description = {0};
    XrXirStatus status = xr_xir_compile_type_description(&ctx->compile, &ctx->types,
        receiver.type, &description);
    if (status == XR_XIR_OK && (description.kind != XR_XIR_TYPE_NOMINAL ||
        description.nominal_kind != XR_XIR_NOMINAL_ENUM)) status = XR_XIR_BAD_TYPE;
    if (status != XR_XIR_OK)
        return source_fail(ctx, node, status, status == XR_XIR_BUDGET ?
            "enum identity budget exhausted" : "enum identity requires a declared enum type");
    uint32_t owner = description.declaration;
    uint32_t count = description.variant_count;
    uint32_t member = ctx->nominal_variants[owner][count + (qualified ? 2 : 1)];
    if (!source_query_target_token_reference(ctx,node, member,
        qualified ? XR_XIR_SOURCE_CALL : XR_XIR_SOURCE_READ)) return false;
    SourceValue tag;
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_ENUM_TAG, XR_XIR_I64, {receiver.id,0}, {0}, 0, {0}}, &tag) ||
        !source_enum_text_literal(ctx, node, &description, count - 1, qualified, value)) return false;
    for (uint32_t variant = 0; variant + 1 < count; ++variant) {
        SourceValue ordinal, condition, selected;
        if (!((ctx)->diagnostic.status == XR_XIR_OK) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_CONST_INT, XR_XIR_I64, {0}, {0}, variant, {0}}, &ordinal) ||
            !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_EQ_INT, XR_XIR_BOOL, {tag.id,ordinal.id}, {0}, 0, {0}}, &condition)) return false;
        SourceFunction *body = &ctx->bodies[ctx->function];
        uint32_t branch = body->count, yes = body->block_count;
        if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_BRANCH, XR_XIR_UNIT, {condition.id}, {0}, 0, {0}}, NULL) ||
            !begin_block(ctx) || !source_enum_text_literal(ctx, node, &description, variant, qualified, &selected)) return false;
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
        SourceValue inputs[] = {{.id=yes_end,.type=XR_XIR_UNIT,.place={0}}, selected, {.id=no,.type=XR_XIR_UNIT,.place={0}}, *value};
        if (!source_recipe_group(ctx, (XrXirInstruction){XR_XIR_PHI, XR_XIR_STRING, {0}, {0}, 0, {0}}, inputs, 4, value)) return false;
    }
    return true;
}
