/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_print_nullable.inc.c - Nullable string display through owned String values
 *
 * KEY CONCEPT:
 *   The argument has already been evaluated. Only its outer presence selects
 *   the String value supplied to PRINT, without broadening the output protocol.
 */
static bool source_print_nullable(SourceContext *ctx, AstNode *node, SourceValue *value) {
    if (!xr_xir_type_is_nullable(&ctx->types, value->type) ||
        xr_xir_nullable_element(&ctx->types, value->type) != XR_XIR_STRING) return true;
    SourceValue argument = *value, present, some, none;
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_NULLABLE_IS_SOME,
        XR_XIR_BOOL, {argument.id, 0}, {0}, 0, {0}}, &present)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    uint32_t branch = body->count, yes_block = body->block_count;
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_BRANCH, XR_XIR_UNIT,
            {present.id, 0}, {0}, 0, {0}}, NULL) || !begin_block(ctx) ||
        !source_recipe_record(ctx, (XrXirInstruction){XR_XIR_NULLABLE_UNWRAP,
            XR_XIR_STRING, {argument.id, 0}, {0}, 0, {0}}, &some)) return false;
    uint32_t yes_end = body->current_block.identity, yes_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_JUMP, XR_XIR_UNIT,
            {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t no_block = body->block_count;
    if (!begin_block(ctx) || !source_string_literal(ctx, node, "null", 4, &none)) return false;
    uint32_t no_end = body->current_block.identity, no_jump = body->count;
    if (!source_recipe_record(ctx, (XrXirInstruction){XR_XIR_JUMP, XR_XIR_UNIT,
            {0}, {0}, 0, {0}}, NULL)) return false;
    uint32_t join = body->block_count;
    if (!begin_block(ctx)) return false;
    body->recipes[branch].instruction.targets[0] = yes_block;
    body->recipes[branch].instruction.targets[1] = no_block;
    body->recipes[yes_jump].instruction.targets[0] = join;
    body->recipes[no_jump].instruction.targets[0] = join;
    SourceValue inputs[] = {{.id=yes_end, .type=XR_XIR_UNIT,.place={0}}, some, {.id=no_end, .type=XR_XIR_UNIT,.place={0}}, none};
    return source_recipe_group(ctx, (XrXirInstruction){XR_XIR_PHI, XR_XIR_STRING,
        {0}, {0}, 0, {0}}, inputs, 4, value);
}
