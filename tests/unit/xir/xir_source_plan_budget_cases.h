/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_plan_budget_cases.h - Private planning rollback and exact source_fixture_compile_live storage
 *
 * KEY CONCEPT:
 *   Context and conversion planning cannot publish instructions before success.
 */
static void source_region_snapshot_identity_cases(void) {
    SourceContext ctx={0}; CHECK(allocation_private_context(&ctx,allocation_limits()));
    XrXirFunction function={0}; function.parameter_count=1;
    SourceFunction body={0}; ctx.bodies=&body; ctx.functions=&function;
    source_fixture_compile_attempts=0; source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(source_recipe_record(&ctx,(XrXirInstruction){XR_XIR_LOCAL_WRITE,XR_XIR_UNIT,
        {0,UINT32_MAX},{0},0,{0}},NULL));
    SourceValue placeholder={UINT32_MAX,XR_XIR_STRING};
    CHECK(source_recipe_group(&ctx,(XrXirInstruction){XR_XIR_CALL,XR_XIR_STRING,
        {0},{0},0,{0}},&placeholder,1,NULL));
    XrXirFunction snapshot=function;

    CHECK(source_region_emit(&ctx,&snapshot,false));
    CHECK(snapshot.instructions[0].args[1]==UINT32_MAX && snapshot.operands[0]==UINT32_MAX);
    CHECK(!body.region_sealed);
    XrXirFunction output=function;
    CHECK(!source_region_emit(&ctx,&output,true));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !output.instructions && !output.blocks && !output.operands);
    CHECK(!body.region_sealed);
    ctx.diagnostic.status=XR_XIR_OK;
    body.recipes[0].instruction.args[1]=UINT32_MAX-1;
    CHECK(!source_region_emit(&ctx,&output,false));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !output.instructions);
    body.recipes[0].instruction.args[1]=UINT32_MAX;body.recipes[0].owner=1;
    ctx.diagnostic.status=XR_XIR_OK;
    CHECK(!source_region_emit(&ctx,&output,false));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_STRUCTURE && !output.instructions);
    allocation_private_close(&ctx);
    CHECK(!source_fixture_compile_live);source_fixture_compile_attempts=0;source_fixture_compile_fail_at=SIZE_MAX;
}
static void source_plan_context_cases(void) {
    source_region_snapshot_identity_cases();
    SourceContext ctx = {0}; CHECK(allocation_private_context(&ctx,allocation_limits()));
    SourceConversionRecipe recipe = {0};
    CHECK(source_conversion_plan(&ctx,NULL,XR_XIR_I8,(SourceExpectedType){false,XR_XIR_UNIT,false,false},&recipe));
    CHECK(!recipe.needed && recipe.source == XR_XIR_I8 && recipe.target == XR_XIR_I8);
    CHECK(source_conversion_plan(&ctx,NULL,XR_XIR_UNIT,(SourceExpectedType){true,XR_XIR_UNIT,false,false},&recipe));
    CHECK(!recipe.needed && recipe.target == XR_XIR_UNIT);
    SourceConversionRecipe before = recipe;
    CHECK(!source_conversion_plan(&ctx,NULL,XR_XIR_I8,(SourceExpectedType){true,XR_XIR_UNIT,false,false},&recipe));
    CHECK(ctx.diagnostic.status == XR_XIR_BAD_TYPE && recipe.target == before.target && recipe.needed == before.needed);
    ctx.diagnostic.status = XR_XIR_OK;
    CHECK(source_conversion_plan(&ctx,NULL,XR_XIR_I8,(SourceExpectedType){true,XR_XIR_I64,false,false},&recipe));
    CHECK(recipe.needed && recipe.operation == XR_XIR_CONVERT_NUMBER && recipe.target == XR_XIR_I64);
    before = recipe;
    CHECK(!source_conversion_plan(&ctx,NULL,XR_XIR_I64,(SourceExpectedType){true,XR_XIR_I8,false,false},&recipe));
    CHECK(recipe.source == before.source && recipe.target == before.target);
    CHECK(!ctx.memory && !ctx.query.expression_count);
    SourceInteger literal = {true,false,41};
    SourceNumericRequest request = {&literal,NULL,{false,XR_XIR_UNIT,false,false},false};
    SourceNumericRecipe numeric = {0}; ctx.diagnostic.status = XR_XIR_OK;
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric) && !numeric.ready);
    request.expected = (SourceExpectedType){true,XR_XIR_I8,false,false};
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric) && numeric.ready && numeric.type == XR_XIR_I8 && numeric.payload == 41);
    request.expected=(SourceExpectedType){false,(XrXirType)UINT32_MAX,false,false};
    request.permit_default=true;
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric) && numeric.ready && numeric.type==XR_XIR_I64);
    request.expected=(SourceExpectedType){false,XR_XIR_F32,false,false};
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric) && numeric.type==XR_XIR_I64);
    SourceConditionalRecipe conditional={0};
    CHECK(source_conditional_plan(&ctx,NULL,XR_XIR_I8,XR_XIR_I64,
        (SourceExpectedType){false,XR_XIR_UNIT,false,false},&conditional) && conditional.result==XR_XIR_I64);
    CHECK(source_conditional_plan(&ctx,NULL,XR_XIR_I8,XR_XIR_I64,
        (SourceExpectedType){false,(XrXirType)UINT32_MAX,false,false},&conditional) && conditional.result==XR_XIR_I64);
    CHECK(source_conditional_plan(&ctx,NULL,XR_XIR_UNIT,XR_XIR_UNIT,
        (SourceExpectedType){true,XR_XIR_UNIT,false,false},&conditional) && conditional.result==XR_XIR_UNIT);
    CHECK(!source_conditional_plan(&ctx,NULL,XR_XIR_I64,XR_XIR_I64,
        (SourceExpectedType){true,XR_XIR_UNIT,false,false},&conditional));
    ctx.diagnostic.status=XR_XIR_OK;
    XrXirTypeNode promises[2]={
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,XR_XIR_CALLABLE_ROOT_UNRESOLVED,0,{0}},
        {XR_XIR_TYPE_CALLABLE,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,XR_XIR_CALLABLE_NO_SUSPEND|XR_XIR_CALLABLE_ROOT_UNRESOLVED,0,{0}}};
    XrXirFunctionIdentity identity={0}; ctx.identities=&identity;
    ctx.types=(XrXirTypes){promises,2,NULL,NULL};
    XrXirType target=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    CHECK(source_reference_promise(&ctx,NULL,0,
        (SourceExpectedType){false,(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+1),false,false},&target));
    CHECK(target==(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE && !ctx.memory);
    ctx.types=(XrXirTypes){0};
    request.expected=(SourceExpectedType){true,XR_XIR_I8,false,false};
    CHECK(source_numeric_plan(&ctx,NULL,&request,&numeric));
    SourceNumericRecipe saved = numeric; literal.magnitude = 128;
    CHECK(!source_numeric_plan(&ctx,NULL,&request,&numeric));
    CHECK(ctx.diagnostic.status == XR_XIR_BAD_TYPE && numeric.type == saved.type && numeric.payload == saved.payload);
    CHECK(!ctx.memory && !ctx.query.expression_count);
    allocation_private_close(&ctx);CHECK(!source_fixture_compile_live);
}
static void source_plan_conversion_budget_cases(void) {
    XrXirCallableParameter parameters[]={{XR_XIR_I64,0},{XR_XIR_I64,0}};
    SourceValue values[]={{0,XR_XIR_I64},{1,XR_XIR_I64}};
    SourceCallConversions request={SOURCE_CALL_REQUIREMENT,{0},NULL,parameters,values,2,0};
    SourceContext ctx={0};CHECK(allocation_private_context(&ctx,allocation_limits()));
    size_t start=source_fixture_compile_attempts;
    CHECK(source_call_conversions(&ctx,NULL,&request));
    XrCompileResourceStats fees={0};CHECK(xr_compile_resources_stats(ctx.compile.resources,&fees)==XR_COMPILE_RESOURCE_OK);
    CHECK(fees.work==3 && source_fixture_compile_attempts==start && !ctx.memory);
    allocation_private_close(&ctx);CHECK(!source_fixture_compile_live);
    uint64_t scratch=sizeof(SourceConversionRecipe);
    for(unsigned role=0;role<4;++role){
        values[0]=(SourceValue){0,XR_XIR_I64};values[1]=(SourceValue){1,XR_XIR_I8};
        XrCompileResourceLimits caps=allocation_limits();
        if(role==1)caps.live_bytes=sizeof(XrCompileResources)+sizeof(CompileAllocation)+scratch-1;
        if(role==2)caps.allocated_bytes=sizeof(XrCompileResources)+sizeof(CompileAllocation)+scratch-1;
        if(role==3)caps.work=2;
        ctx=(SourceContext){0};CHECK(allocation_private_context(&ctx,caps));
        start=source_fixture_compile_attempts;source_fixture_compile_fail_at=role==0?start:SIZE_MAX;
        source_fixture_compile_injected=false;
        CHECK(!source_call_conversions(&ctx,NULL,&request));
        CHECK(ctx.diagnostic.status==(role?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY));
        CHECK(!ctx.memory && values[1].type==XR_XIR_I8 && values[1].id==1);
        CHECK(source_fixture_compile_attempts==start+(role==0));
        allocation_private_close(&ctx);CHECK(!source_fixture_compile_live);
        if(role==0)allocation_pure_point("conversion_recipe",0);
    }
    source_fixture_compile_fail_at=SIZE_MAX;
    ctx=(SourceContext){0};CHECK(allocation_private_context(&ctx,allocation_limits()));
    XrXirFunction function={0};function.parameter_count=2;SourceFunction body={0};
    ctx.bodies=&body;ctx.functions=&function;ctx.remaining_blocks=1;ctx.remaining_instructions=1;
    CHECK(source_call_conversions(&ctx,NULL,&request));
    CHECK(body.count==1 && body.recipes[0].instruction.op==XR_XIR_CONVERT_NUMBER);
    CHECK(body.recipes[0].owner==ctx.function && body.recipes[0].value==2);
    CHECK(body.recipe_storage && body.block_count==1 && !body.region_sealed);
    CHECK(values[1].type==XR_XIR_I64 && values[1].id==2 && source_fixture_compile_live==3);
    allocation_private_close(&ctx);CHECK(!source_fixture_compile_live);
    ctx=(SourceContext){0};CHECK(allocation_private_context(&ctx,allocation_limits()));
    body=(SourceFunction){0};ctx.bodies=&body;ctx.functions=&function;
    values[0]=(SourceValue){0,XR_XIR_I8};values[1]=(SourceValue){1,XR_XIR_I64};parameters[1].type=XR_XIR_I8;
    CHECK(!source_call_conversions(&ctx,NULL,&request));
    CHECK(ctx.diagnostic.status==XR_XIR_BAD_TYPE);
    CHECK(!body.count && !values[0].id && values[1].id==1);
    CHECK(values[0].type==XR_XIR_I8 && values[1].type==XR_XIR_I64 && !ctx.memory && !ctx.query.expression_count);
    allocation_private_close(&ctx);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    source_fixture_compile_fail_at=SIZE_MAX;
}
