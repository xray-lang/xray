/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_calls_cases.h - Real Source finite scalar call-stack controls
 *
 * KEY CONCEPT:
 *   Public receiving transitions outlive all producers before evaluation.
 *   Independent Source results and whole-stack boundaries carry the oracle.
 */
#ifndef XIR_CTFE_CALLS_CASES_H
#define XIR_CTFE_CALLS_CASES_H
#include "xir_ctfe_calls_chain.h"
#include "xir/xxir_scalar_core.h"
static void ctfe_calls_fixture_new(CtfeFixture *fixture) {
    CHECK(!fixture->context.resources && !fixture->lowered);
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&ctfe_owner_limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    XrXirStatus status=ctfe_calls_fixture_build(&context,fixture,NULL);
    if (status!=XR_XIR_OK) xr_compile_resources_release(context.resources);
    CHECK(status==XR_XIR_OK);fixture->owns_resources=true;
}
static XrXirCtfeCallRequest ctfe_calls_request(const CtfeFixture *fixture,const char *name,
    const XrXirValue *arguments,uint32_t count) {
    XrXirCtfeCallRequest request={ctfe_request(fixture,name,arguments,count),XR_XIR_CTFE_MAX_CALL_DEPTH};
    return request;
}
static void ctfe_calls_expect(CtfeFixture *fixture,const XrXirCtfeCallRequest *request,
    XrXirCtfeStatus expected,int64_t payload) {
    XrCompileResourceStats before=ctfe_stats(fixture);XrXirValue value={0};
    size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCtfeStatus actual=xr_xir_compile_ctfe_calls(request,&value);
    if (actual!=expected) fprintf(stderr,"calls f%u status%u expected%u\n",
        request->evaluation.function,(unsigned)actual,(unsigned)expected);
    CHECK(actual==expected);
    CHECK(value.type==(uint32_t)(expected==XR_XIR_CTFE_OK?XR_XIR_I64:XR_XIR_UNIT) && !value.reserved && value.payload==payload);
    XrCompileResourceStats after=ctfe_stats(fixture);
    CHECK(after.live_bytes==before.live_bytes);ctfe_stats_monotonic(before,after);
    CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
}
static uint64_t ctfe_calls_straight_steps(const CtfeFixture *fixture) {
    const char *const names[]={"answer","middle","leaf"};uint64_t steps=0;
    const XrXirModule *module=xr_xir_compile_artifact_module(fixture->lowered);
    for (uint32_t n=0;n<3;++n) {
        XrXirCtfeRequest request=ctfe_request(fixture,names[n],NULL,0);
        const XrXirFunction *body=&module->functions[request.function];
        CHECK(body->block_count==1);steps+=body->instruction_count;
    }
    CHECK(steps>3 && steps<=XR_XIR_CTFE_MAX_STEPS);return steps;
}
static void ctfe_calls_boundaries(CtfeFixture *fixture) {
    XrXirCtfeCallRequest request=ctfe_calls_request(fixture,"answer",NULL,0);
    request.call_depth=3;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,42);
    request.call_depth=2;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_DEPTH_LIMIT,0);
    request.call_depth=0;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_DEPTH_LIMIT,0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);
    request.evaluation.limits.steps=ctfe_calls_straight_steps(fixture);
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,42);
    --request.evaluation.limits.steps;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_STEP_LIMIT,0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);
    request.evaluation.limits.output_bytes=7;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OUTPUT_LIMIT,0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);
    request.evaluation.limits.frame_bytes=0;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_FRAME_LIMIT,0);
    /* Locate physical bookkeeping independently of its private layout. Every
     * probe is a fresh full request on the same original finite ledger. */
    uint64_t low=0,high=XR_XIR_CTFE_MAX_FRAME_BYTES;
    while (low+1<high) {
        uint64_t middle=low+(high-low)/2;request.evaluation.limits.frame_bytes=middle;
        XrXirValue value={0};XrCompileResourceStats before=ctfe_stats(fixture);
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&value);
        CHECK(status==XR_XIR_CTFE_OK || status==XR_XIR_CTFE_FRAME_LIMIT);
        CHECK(ctfe_stats(fixture).live_bytes==before.live_bytes);
        if (status==XR_XIR_CTFE_OK) { CHECK(value.type==XR_XIR_I64 && value.payload==42);high=middle; }
        else { CHECK(!value.type && !value.reserved && !value.payload);low=middle; }
    }
    request.evaluation.limits.frame_bytes=high;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,42);
    request.evaluation.limits.frame_bytes=high-1;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_FRAME_LIMIT,0);
    fprintf(stderr,"CTFE calls whole-stack activation boundary=%llu exact/minus1\n",(unsigned long long)high);
}
static void ctfe_calls_mutations(CtfeFixture *fixture) {
    XrXirCtfeCallRequest request=ctfe_calls_request(fixture,"answer",NULL,0);
    XrXirFunction *body=&((XrXirFunction *)fixture->lowered->module.functions)[request.evaluation.function];
    XrXirInstruction *instructions=(XrXirInstruction *)body->instructions;
    uint32_t call=UINT32_MAX;
    for (uint32_t i=0;i<body->instruction_count;++i) if (instructions[i].op==XR_XIR_CALL) { CHECK(call==UINT32_MAX);call=i; }
    CHECK(call!=UINT32_MAX);XrXirInstruction original=instructions[call];
    instructions[call].immediate=-1;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARTIFACT,0);instructions[call]=original;
    instructions[call].args[1]=UINT32_MAX;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARTIFACT,0);instructions[call]=original;
    instructions[call].type=XR_XIR_BOOL;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARTIFACT,0);instructions[call]=original;
    XrXirFunction *target=&((XrXirFunction *)fixture->lowered->module.functions)[(uint32_t)original.immediate];
    XrXirType saved=target->result;target->result=XR_XIR_BOOL;
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARTIFACT,0);target->result=saved;
    CHECK(xr_xir_compile_artifact_verify(fixture->lowered,NULL)==XR_XIR_OK);
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,42);
    XrXirValue occupied={XR_XIR_I64,77,99},copy=occupied;
    XrCompileResourceStats before=ctfe_stats(fixture);size_t attempts=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_calls(&request,&occupied)==XR_XIR_CTFE_BAD_ARGUMENT);
    CHECK(!memcmp(&occupied,&copy,sizeof(copy)) && attempts==source_fixture_compile_attempts);
    XrCompileResourceStats after=ctfe_stats(fixture);CHECK(!memcmp(&before,&after,sizeof(before)));
    request.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH+1;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARGUMENT,0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);request.evaluation.checked_identity[0]^=1;
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_IDENTITY,0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);request.evaluation.target.abi_version^=1;
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_PROFILE,0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);request.evaluation.limits.steps=XR_XIR_CTFE_MAX_STEPS+1;
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARGUMENT,0);
    const XrXirDeclarations *declarations=fixture->lowered->module.declarations;
    CHECK(declarations->module_count>0);
    request=ctfe_calls_request(fixture,"answer",NULL,0);request.evaluation.function=declarations->modules[0].initializer;
    CHECK(request.evaluation.function<fixture->lowered->module.function_count);
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_NOT_SUPPORTED,0);
    bool cleanup=false;
    for (uint32_t f=0;f<fixture->lowered->module.function_count;++f) {
        if (!declarations->functions[f].cleanup_owner) continue;
        request.evaluation.function=f;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_NOT_SUPPORTED,0);cleanup=true;
    }
    CHECK(cleanup);
}
static void ctfe_calls_equal_cases(CtfeFixture *fixture) {
    XrXirValue args[]={{XR_XIR_BOOL,0,1},{XR_XIR_I64,0,INT64_MIN},{XR_XIR_I64,0,INT64_MIN}};
    XrXirCtfeCallRequest request=ctfe_calls_request(fixture,"compare",args,3);
    const XrXirFunction *body=&fixture->lowered->module.functions[request.evaluation.function];
    uint32_t equal_ops=0,not_equal_ops=0;
    for (uint32_t i=0;i<body->instruction_count;++i) if (body->instructions[i].op==XR_XIR_EQUAL) {
        equal_ops+=body->instructions[i].immediate==0;not_equal_ops+=body->instructions[i].immediate==1;
    }
    CHECK(equal_ops && not_equal_ops);
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,41);
    ctfe_expect(fixture,request.evaluation,XR_XIR_CTFE_OK,XR_XIR_I64,41);
    args[2].payload=INT64_MAX;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,0);
    args[0].payload=0;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,41);
    args[2].payload=INT64_MIN;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,0);
    args[0].payload=2;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_BAD_ARGUMENT,0);
    XrXirValue wide[]={{XR_XIR_U64,0,-1},{XR_XIR_U64,0,-1}};
    request=ctfe_calls_request(fixture,"unsigned_equal",wide,2);
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,41);
    wide[1].payload=INT64_MAX;ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_OK,0);
    request=ctfe_calls_request(fixture,"managed_equal",NULL,0);
    ctfe_calls_expect(fixture,&request,XR_XIR_CTFE_NOT_SUPPORTED,0);
    bool retained=true;
    CHECK(xir_scalar_core_equal(XR_XIR_BOOL,2,0,&retained)==XR_XIR_RUN_BAD_ARGUMENT && retained);
    CHECK(xir_scalar_core_equal(XR_XIR_I8,128,0,&retained)==XR_XIR_RUN_BAD_ARGUMENT && retained);
    CHECK(xir_scalar_core_equal(XR_XIR_STRING,0,0,&retained)==XR_XIR_RUN_BAD_ARGUMENT && retained);
    CHECK(xir_scalar_core_equal(XR_XIR_U64,-1,-1,&retained)==XR_XIR_RUN_OK && retained);
    CHECK(xir_scalar_core_equal(XR_XIR_I8,-128,127,&retained)==XR_XIR_RUN_OK && !retained);
}

static void ctfe_calls_semantics(void) {
    CtfeFixture fixture={0};ctfe_calls_fixture_new(&fixture);
    XrXirCtfeCallRequest request=ctfe_calls_request(&fixture,"answer",NULL,0);
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,42);
    XrXirValue logical={XR_XIR_I64,0,40};request=ctfe_calls_request(&fixture,"logic",&logical,1);
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,42);logical.payload=43;
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,1);
    XrXirValue argument={XR_XIR_I64,0,3};request=ctfe_calls_request(&fixture,"recurse",&argument,1);
    request.call_depth=4;ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,41);
    request.call_depth=3;ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_DEPTH_LIMIT,0);
    argument.payload=-1;request.call_depth=4;ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_DEPTH_LIMIT,0);
    request=ctfe_calls_request(&fixture,"endless",NULL,0);request.call_depth=3;
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_DEPTH_LIMIT,0);
    request.call_depth=128;request.evaluation.limits.steps=2;ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_STEP_LIMIT,0);
    request=ctfe_calls_request(&fixture,"division",NULL,0);ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_DIVIDE_BY_ZERO,0);
    request=ctfe_calls_request(&fixture,"unsupported",NULL,0);ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_NOT_SUPPORTED,0);
    request=ctfe_calls_request(&fixture,"deferred",NULL,0);ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_NOT_SUPPORTED,0);
    request=ctfe_calls_request(&fixture,"managed",NULL,0);ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_NOT_SUPPORTED,0);
    argument=(XrXirValue){XR_XIR_BOOL,0,1};request=ctfe_calls_request(&fixture,"choose",&argument,1);
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,42);argument.payload=0;
    ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,1);
    request=ctfe_calls_request(&fixture,"loop",&argument,1);ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_OK,1);
    argument.payload=1;request.evaluation.limits.steps=32;ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_STEP_LIMIT,0);
    argument.payload=2;ctfe_calls_expect(&fixture,&request,XR_XIR_CTFE_BAD_ARGUMENT,0);
    ctfe_calls_equal_cases(&fixture);ctfe_calls_boundaries(&fixture);ctfe_calls_mutations(&fixture);
    request=ctfe_calls_request(&fixture,"answer",NULL,0);XrXirValue retained={0};
    CHECK(xr_xir_compile_ctfe_calls(&request,&retained)==XR_XIR_CTFE_OK);ctfe_fixture_free(&fixture);
    CHECK(retained.type==XR_XIR_I64 && !retained.reserved && retained.payload==42);
    fprintf(stderr,"CTFE direct CALL real Source/packet/specialization/Lowered, recursive/PHI, whole steps/depth/bytes, malformed closure and producer death PASS\n");
}
#endif // XIR_CTFE_CALLS_CASES_H
