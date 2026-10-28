/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_try_assert.inc.c - Lexical error-to-panic expression continuation
 *
 * KEY CONCEPT:
 *   The operand retains its declared error channel until its actual invoke
 *   landing converts the owned error. Nested functions have separate bodies.
 */
static bool source_try_assert(SourceContext *ctx, SourceExpressionPlan *operand, SourceValue *value) {
    uint32_t owner=ctx->function;
    SourceErrorContext *outer=ctx->bodies[owner].error_context;
    SourceErrorContext handler={.frontier=ctx->bodies[owner].frontier,.asserted=true};
    ctx->bodies[owner].error_context=&handler;
    bool complete=source_plan_complete(ctx,operand,value);
    ctx->bodies[owner].error_context=outer;
    return complete;
}
