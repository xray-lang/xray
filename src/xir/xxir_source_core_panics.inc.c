/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_core_panics.inc.c - Typed action-only panic protection
 */
static bool source_core_panics_body(SourceContext *ctx) {
    if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_JUMP,XR_XIR_UNIT,{0},{1,0},0,{0}},NULL) || !begin_block(ctx)) return false;
    SourceFunction *body = &ctx->bodies[ctx->function];
    body->blocks[1].block.panic = 4;
    if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_INVOKE_INDIRECT,XR_XIR_TYPE_PARAMETER_BASE,{0},{2,3},0,{0}},NULL) ||
        !begin_block(ctx) || !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_INVOKE_DISCARD,XR_XIR_UNIT,{0},{0},1,{0}},NULL)) return false;
    SourceValue condition;
    if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_CONST_BOOL,XR_XIR_BOOL,{0},{0},0,{0}},&condition) ||
        !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_ASSERT_CONDITION,XR_XIR_UNIT,{condition.id,1},{0},0,{0}},NULL) ||
        !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},NULL) || !begin_block(ctx)) return false;
    SourceValue error;
    if (!source_recipe_append(ctx,(XrXirInstruction){XR_XIR_INVOKE_ERROR,XR_XIR_ERROR,{0},{0},1,{0}},&error) ||
        !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_THROW,XR_XIR_UNIT,{error.id,0},{0},0,{0}},NULL) || !begin_block(ctx) ||
        !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_PANIC_CATCH,XR_XIR_UNIT,{0},{0},0,{0}},NULL) ||
        !source_recipe_append(ctx,(XrXirInstruction){XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}},NULL)) return false;
    ctx->returned = true;
    return true;
}
