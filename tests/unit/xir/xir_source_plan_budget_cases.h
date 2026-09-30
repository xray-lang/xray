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
static void source_region_snapshot_identity_cases(void) {
    SourceContext ctx={0}; ctx.budget=xr_xir_default_budget();
    XrXirFunction function={0}; function.parameter_count=1;
    SourceFunction body={0}; ctx.bodies=&body; ctx.functions=&function;
    attempts=0; fail_at=SIZE_MAX;
    CHECK(source_recipe_record(&ctx,(XrXirInstruction){XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,
        {0,UINT32_MAX},{0},0,{0}},NULL));
    SourceValue placeholder={UINT32_MAX,XR_XIR_STRING};
    CHECK(source_recipe_group(&ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_STRING,
        {0},{0},0,{0}},&placeholder,1,NULL));
    XrXirFunction snapshot=function;
    uint64_t scratch=ctx.budget.scratch_bytes;
    CHECK(source_region_emit(&ctx,&snapshot,false));
    CHECK(snapshot.instructions[0].args[1]==UINT32_MAX && snapshot.operands[0]==UINT32_MAX);
    CHECK(!body.region_sealed && ctx.budget.scratch_bytes==scratch);
    XrXirFunction output=function;
    CHECK(!source_region_emit(&ctx,&output,true));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !output.instructions && !output.blocks && !output.operands);
    CHECK(!body.region_sealed && ctx.budget.scratch_bytes==scratch);
    ctx.diagnostic.status=XR_XIR_OK;
    body.recipes[0].instruction.args[1]=UINT32_MAX-1;
    CHECK(!source_region_emit(&ctx,&output,false));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !output.instructions && ctx.budget.scratch_bytes==scratch);
    body.recipes[0].instruction.args[1]=UINT32_MAX;body.recipes[0].owner=1;
    ctx.diagnostic.status=XR_XIR_OK;
    CHECK(!source_region_emit(&ctx,&output,false));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !output.instructions && ctx.budget.scratch_bytes==scratch);
    while(ctx.memory){SourceMemory *next=ctx.memory->next;xr_free(ctx.memory);ctx.memory=next;}
    CHECK(!live);attempts=0;fail_at=SIZE_MAX;
}
static void source_plan_context_cases(void) {
    source_region_snapshot_identity_cases();
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
    XrXirFunction function = {0}; function.parameter_count = 2;
    SourceFunction body = {0};
    ctx.budget.metadata_bytes = xr_xir_default_budget().metadata_bytes;
    ctx.budget.blocks = 1;
    ctx.bodies = &body; ctx.functions = &function; ctx.budget.instructions = 1;
    ctx.budget.work = 20; ctx.diagnostic.status = XR_XIR_OK;
    CHECK(source_call_conversions(&ctx,NULL,&request));
    CHECK(body.count == 1 && body.recipes[0].instruction.op == XR_XIR_CONVERT_NUMBER);
    CHECK(body.recipes[0].owner == ctx.function && body.recipes[0].value == 2);
    CHECK(body.recipe_storage && body.block_count == 1 && !body.region_sealed);
    CHECK(values[1].type == XR_XIR_I64 && values[1].id == 2 && ctx.budget.scratch_bytes == scratch && live == 2);
    while (ctx.memory) {
        SourceMemory *next = ctx.memory->next; xr_free(ctx.memory); ctx.memory = next;
    }
    CHECK(!live); body = (SourceFunction){0};
    ctx.diagnostic.status = XR_XIR_OK; ctx.budget.work = 20;
    values[0] = (SourceValue){0,XR_XIR_I8}; values[1] = (SourceValue){1,XR_XIR_I64};
    parameters[1].type = XR_XIR_I8; ctx.budget.scratch_bytes = scratch*2;
    CHECK(!source_call_conversions(&ctx,NULL,&request));
    CHECK(ctx.diagnostic.status == XR_XIR_BAD_TYPE && ctx.budget.scratch_bytes == scratch*2 && ctx.budget.work == 18);
    CHECK(!body.count && !values[0].id && values[1].id == 1);
    CHECK(values[0].type == XR_XIR_I8 && values[1].type == XR_XIR_I64 && !ctx.memory && !ctx.query.expression_count && !live);
    attempts = 0; fail_at = SIZE_MAX;
}
