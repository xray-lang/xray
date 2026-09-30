/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_plan_budget_cases.h - Private planning rollback and exact live storage
 *
 * KEY CONCEPT:
 *   Context and conversion planning cannot publish instructions before success.
 */
static void source_plan_context_cases(void) {
    SourceContext ctx = {0}; ctx.budget = xr_xir_default_budget();
    SourceConversionRecipe recipe = {0};
    CHECK(source_conversion_plan(&ctx,NULL,XR_XIR_I8,(SourceExpectedType){false,XR_XIR_UNIT},&recipe));
    CHECK(!recipe.needed && recipe.source == XR_XIR_I8 && recipe.target == XR_XIR_I8);
    CHECK(source_conversion_plan(&ctx,NULL,XR_XIR_UNIT,(SourceExpectedType){true,XR_XIR_UNIT},&recipe));
    CHECK(!recipe.needed && recipe.target == XR_XIR_UNIT);
    SourceConversionRecipe before = recipe;
    CHECK(!source_conversion_plan(&ctx,NULL,XR_XIR_I8,(SourceExpectedType){true,XR_XIR_UNIT},&recipe));
    CHECK(ctx.diagnostic.status == XR_XIR_BAD_TYPE && recipe.target == before.target && recipe.needed == before.needed);
    ctx.diagnostic.status = XR_XIR_OK;
    CHECK(source_conversion_plan(&ctx,NULL,XR_XIR_I8,(SourceExpectedType){true,XR_XIR_I64},&recipe));
    CHECK(recipe.needed && recipe.operation == XR_XIR_CONVERT_NUMBER && recipe.target == XR_XIR_I64);
    before = recipe;
    CHECK(!source_conversion_plan(&ctx,NULL,XR_XIR_I64,(SourceExpectedType){true,XR_XIR_I8},&recipe));
    CHECK(recipe.source == before.source && recipe.target == before.target);
    CHECK(!ctx.memory && !ctx.query.expression_count);
    SourceInteger literal = {true,false,41};
    SourceNumericRequest request = {&literal,NULL,{false,XR_XIR_UNIT},false};
    SourceNumericRecipe numeric = {0}; ctx.diagnostic.status = XR_XIR_OK;
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric) && !numeric.ready);
    request.expected = (SourceExpectedType){true,XR_XIR_I8};
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric) && numeric.ready && numeric.type == XR_XIR_I8 && numeric.payload == 41);
    SourceNumericRecipe saved = numeric; literal.magnitude = 128;
    CHECK(!source_numeric_plan(&ctx,NULL,&request,&numeric));
    CHECK(ctx.diagnostic.status == XR_XIR_BAD_TYPE && numeric.type == saved.type && numeric.payload == saved.payload);
    CHECK(!ctx.memory && !ctx.query.expression_count);
}
static void source_plan_conversion_budget_cases(void) {
    SourceContext ctx = {0}; ctx.budget.work = 20;
    XrXirCallableParameter parameters[] = {{XR_XIR_I64,0},{XR_XIR_I64,0}};
    SourceValue values[] = {{0,XR_XIR_I64},{1,XR_XIR_I64}};
    SourceCallConversions request = {SOURCE_CALL_REQUIREMENT,{0},NULL,parameters,values,2,0};
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(source_call_conversions(&ctx,NULL,&request));
    CHECK(!ctx.budget.scratch_bytes && !attempts && ctx.budget.work == 18);
    values[1].type = XR_XIR_I8;
    uint64_t scratch = sizeof(SourceConversionRecipe);
    ctx.budget.scratch_bytes = scratch; attempts = 0; fail_at = 0;
    CHECK(!source_call_conversions(&ctx,NULL,&request));
    CHECK(ctx.diagnostic.status == XR_XIR_OUT_OF_MEMORY && ctx.budget.scratch_bytes == scratch && !live);
    fail_at = SIZE_MAX; attempts = 0; ctx.diagnostic.status = XR_XIR_OK;
    ctx.budget.scratch_bytes = scratch-1;
    CHECK(!source_call_conversions(&ctx,NULL,&request));
    CHECK(ctx.diagnostic.status == XR_XIR_BUDGET && !attempts && ctx.budget.scratch_bytes == scratch-1);
    ctx.diagnostic.status = XR_XIR_OK; ctx.budget.work = 1; ctx.budget.scratch_bytes = scratch;
    CHECK(!source_call_conversions(&ctx,NULL,&request));
    CHECK(ctx.diagnostic.status == XR_XIR_BUDGET && !attempts && ctx.budget.work == 1);
    XrXirInstruction ops[2] = {0}; XrXirBlock blocks[1] = {0};
    XrXirFunction function = {0}; function.parameter_count = 2;
    SourceFunction body = {0}; body.ops = ops; body.capacity = 2;
    body.blocks = blocks; body.block_count = 1;
    ctx.bodies = &body; ctx.functions = &function; ctx.budget.instructions = 1;
    ctx.budget.work = 20; ctx.diagnostic.status = XR_XIR_OK;
    CHECK(source_call_conversions(&ctx,NULL,&request));
    CHECK(body.count == 1 && ops[0].op == XR_XIR_CONVERT_NUMBER);
    CHECK(values[1].type == XR_XIR_I64 && values[1].id == 2 && ctx.budget.scratch_bytes == scratch && !live);
    ctx.diagnostic.status = XR_XIR_OK; ctx.budget.work = 20; body.count = 0;
    values[0] = (SourceValue){0,XR_XIR_I8}; values[1] = (SourceValue){1,XR_XIR_I64};
    parameters[1].type = XR_XIR_I8; ctx.budget.scratch_bytes = scratch*2;
    CHECK(!source_call_conversions(&ctx,NULL,&request));
    CHECK(ctx.diagnostic.status == XR_XIR_BAD_TYPE && ctx.budget.scratch_bytes == scratch*2 && ctx.budget.work == 18);
    CHECK(!body.count && !values[0].id && values[1].id == 1);
    CHECK(values[0].type == XR_XIR_I8 && values[1].type == XR_XIR_I64 && !ctx.memory && !ctx.query.expression_count && !live);
    attempts = 0; fail_at = SIZE_MAX;
}
