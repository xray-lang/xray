/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_float_cases.h - Independent IEEE encodings and compiler refusals
 */
#ifndef XIR_CTFE_FLOAT_CASES_H
#define XIR_CTFE_FLOAT_CASES_H
typedef struct CtfeFloatCase {
    const char *function;
    XrXirType first,second,result;
    uint64_t left,right,expected;
    XrXirCtfeStatus status;
} CtfeFloatCase;
static int64_t reference_float_bits(uint64_t bits) { int64_t value=0;memcpy(&value,&bits,sizeof(value));return value; }
static void reference_float_direct(const XrXirCompileContext *context,XrXirSourceResult *source) {
    uint8_t identity[32]={0};XrXirArtifact *lowered=reference_lower(context,source,identity);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    static const CtfeFloatCase cases[]={
        {"add",XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,UINT64_C(0x3ff0000000000000),UINT64_C(0x4000000000000000),UINT64_C(0x4008000000000000),XR_XIR_CTFE_OK},
        {"add",XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,1,1,2,XR_XIR_CTFE_OK},
        {"add",XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,UINT64_C(0x3ff0000000000000),UINT64_C(0x3ca0000000000000),UINT64_C(0x3ff0000000000000),XR_XIR_CTFE_OK},
        {"divide",XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,UINT64_C(0x3ff0000000000000),0,UINT64_C(0x7ff0000000000000),XR_XIR_CTFE_OK},
        {"divide",XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,0,0,UINT64_C(0x7ff8000000000000),XR_XIR_CTFE_OK},
        {"divide",XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,UINT64_C(0x8000000000000000),UINT64_C(0x3ff0000000000000),UINT64_C(0x8000000000000000),XR_XIR_CTFE_OK},
        {"equal",XR_XIR_F64,XR_XIR_F64,XR_XIR_BOOL,UINT64_C(0x8000000000000000),0,1,XR_XIR_CTFE_OK},
        {"equal",XR_XIR_F64,XR_XIR_F64,XR_XIR_BOOL,UINT64_C(0x7ff0000000000001),UINT64_C(0x7ff0000000000001),0,XR_XIR_CTFE_OK},
        {"narrow",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_F32,UINT64_C(0x4170000010000000),0,UINT64_C(0x4b800000),XR_XIR_CTFE_OK},
        {"narrow",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_F32,UINT64_C(0x36a0000000000000),0,1,XR_XIR_CTFE_OK},
        {"narrow",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_F32,UINT64_C(0x3690000000000000),0,0,XR_XIR_CTFE_OK},
        {"integer",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_I64,UINT64_C(0xc045600000000000),0,UINT64_C(0xffffffffffffffd6),XR_XIR_CTFE_OK},
        {"integer",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_I64,UINT64_C(0xc3e0000000000000),0,UINT64_C(0x8000000000000000),XR_XIR_CTFE_OK},
        {"integer",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_UNIT,UINT64_C(0x43e0000000000000),0,0,XR_XIR_CTFE_NUMERIC_RANGE},
        {"integer",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_UNIT,UINT64_C(0x7ff0000000000000),0,0,XR_XIR_CTFE_NUMERIC_RANGE},
        {"integer",XR_XIR_F64,XR_XIR_UNIT,XR_XIR_UNIT,UINT64_C(0x7ff8000000000000),0,0,XR_XIR_CTFE_NUMERIC_RANGE},
        {"integer_float",XR_XIR_I64,XR_XIR_UNIT,XR_XIR_F64,UINT64_C(0x7fffffffffffffff),0,UINT64_C(0x43e0000000000000),XR_XIR_CTFE_OK},
        {"exact",XR_XIR_F32,XR_XIR_UNIT,XR_XIR_F32,UINT64_C(0x7fc00000),0,UINT64_C(0x7fc00000),XR_XIR_CTFE_OK},
        {"exact",XR_XIR_F32,XR_XIR_UNIT,XR_XIR_UNIT,UINT64_C(0x7f800001),0,0,XR_XIR_CTFE_BAD_ARGUMENT},
        {"exact",XR_XIR_F32,XR_XIR_UNIT,XR_XIR_UNIT,UINT64_C(0x13f800000),0,0,XR_XIR_CTFE_BAD_ARGUMENT}
    };
    XrXirValue output={0},args[2]={{0},{0}};
    XrXirCtfeRequest request={.artifact=lowered,.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,8}};
    memcpy(request.checked_identity,identity,sizeof(identity));XrCompileResourceStats baseline=reference_stats(context);
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        const CtfeFloatCase *test=&cases[i];request.function=reference_named(module,test->function);
        args[0]=(XrXirValue){test->first,0,reference_float_bits(test->left)};
        args[1]=(XrXirValue){test->second,0,reference_float_bits(test->right)};
        request.arguments=args;request.argument_count=test->second==XR_XIR_UNIT?1:2;
        XrXirCtfeCallRequest calls={request,1};
        for (unsigned c=0;c<2;++c) {
            CHECK((c?xr_xir_compile_ctfe_calls(&calls,&output):xr_xir_compile_ctfe_leaf(&request,&output))==test->status);
            CHECK(output.type==(uint32_t)test->result && !output.reserved && output.payload==reference_float_bits(test->expected));
            output=(XrXirValue){0};CHECK(reference_stats(context).live_bytes==baseline.live_bytes);
        }
    }
    request.function=reference_named(module,"narrow");request.arguments=args;request.argument_count=1;
    args[0]=(XrXirValue){XR_XIR_F64,0,INT64_C(0x4170000010000000)};request.limits.output_bytes=4;
    size_t first=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_F32 && output.payload==INT64_C(0x4b800000));
    output=(XrXirValue){0};size_t sites=source_fixture_compile_attempts-first;CHECK(sites>0);
    for (size_t fault=0;fault<sites;++fault) {
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=source_fixture_compile_attempts+fault;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUT_OF_MEMORY);
        CHECK(source_fixture_compile_injected && !output.type && !output.reserved && !output.payload);
        CHECK(reference_stats(context).live_bytes==baseline.live_bytes);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.payload==INT64_C(0x4b800000));output=(XrXirValue){0};
    }
    request.limits.output_bytes=3;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.function=reference_named(module,"divide");request.argument_count=2;
    args[0]=(XrXirValue){XR_XIR_F64,0,INT64_MIN};
    args[1]=(XrXirValue){XR_XIR_F64,0,INT64_C(0x3ff0000000000000)};
    request.limits.output_bytes=8;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OK && output.type==XR_XIR_F64 && output.payload==INT64_MIN);
    output=(XrXirValue){0};request.limits.output_bytes=7;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_OUTPUT_LIMIT && !output.type && !output.payload);
    request.limits.output_bytes=8;request.limits.steps=1;
    CHECK(xr_xir_compile_ctfe_leaf(&request,&output)==XR_XIR_CTFE_STEP_LIMIT && !output.type && !output.payload);
    CHECK(reference_stats(context).live_bytes==baseline.live_bytes);xr_xir_compile_artifact_free(lowered);
    fprintf(stderr,"CTFE Float20 independent IEEE rows leaf/calls: ties-even/subnormal/NaN/Infinity/signed-zero/checked conversion; f32 output4/minus3; f64 output8/minus7; evaluator FI=%zu same-owner retry/live baseline; step failure; physical0\n",sites);
}
#endif // XIR_CTFE_FLOAT_CASES_H
