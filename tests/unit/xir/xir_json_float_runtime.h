/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_float_runtime.h - Independent finite JSON floating token expectations
 */
#ifndef XIR_JSON_FLOAT_RUNTIME_H
#define XIR_JSON_FLOAT_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
static XrXirValue json_float_take(XrXirProgram *program,uint32_t entry,const char *text,size_t length) {
    XrXirDomain *domain=NULL;XrXirValue argument={0},output={0};
    if(text){CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);CHECK(xr_xir_string_new(domain,text,length,&argument)==XR_XIR_VALUE_OK);}
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,text?&argument:NULL,text?1:0)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(instance,65536).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return output;
}
static void json_float_expected(const XrXirValue *value,bool expected,uint64_t bits) {
    bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_nullable_view(value,&some,&payload) && some==expected);
    if(some)CHECK(payload && payload->type==XR_XIR_F64 && !payload->reserved && (uint64_t)payload->payload==bits);
    else CHECK(!payload);
}
static XrXirValue json_float_cases(XrXirProgram *program,const uint32_t selection[3]) {
    static const struct {const char *text;bool some;uint64_t bits;} cases[]={
        {"0",true,0},{"-0",true,UINT64_C(0x8000000000000000)},
        {"1",true,UINT64_C(0x3ff0000000000000)},{"1e+000",true,UINT64_C(0x3ff0000000000000)},
        {"1.7976931348623157e308",true,UINT64_C(0x7fefffffffffffff)},
        {"1.7976931348623158e308",true,UINT64_C(0x7fefffffffffffff)},
        {"1.7976931348623159e308",false,0},{"-2e308",false,0},
        {"4.9406564584124654e-324",true,UINT64_C(1)},
        {"2.4703282292062327e-324",true,0},{"2.4703282292062328e-324",true,UINT64_C(1)},
        {"-1e-9999999999",true,UINT64_C(0x8000000000000000)},
        {"0e9999999999",true,0},{"9007199254740993",true,UINT64_C(0x4340000000000000)},
        {"1.00000000000000011102230246251565404236316680908203125",true,UINT64_C(0x3ff0000000000000)},
        {"",false,0},{"01",false,0},{"-01",false,0},{"+1",false,0},{".5",false,0},{"1.",false,0},
        {"1e",false,0},{"1e+",false,0},{"1_0",false,0},{"0x1",false,0},{"nan",false,0},{"inf",false,0},
        {" 1",false,0},{"1 ",false,0},{"1\t",false,0},{"true",false,0},{"1,2",false,0},{"1e9999999999x",false,0}
    };
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirValue answer=json_float_take(program,selection[0],NULL,0);CHECK(answer.type==XR_XIR_I64 && !answer.reserved && answer.payload==42);xr_xir_value_drop(&answer);
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirValue value=json_float_take(program,selection[1],cases[i].text,strlen(cases[i].text));
        json_float_expected(&value,cases[i].some,cases[i].bits);xr_xir_value_drop(&value);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    XrXirValue embedded=json_float_take(program,selection[1],"1\0x",3);json_float_expected(&embedded,false,0);xr_xir_value_drop(&embedded);
    fprintf(stderr,"JSON floating cold library: private binary64 state, %zu exact-bit/grammar cases, embedded NUL and comptime42\n",sizeof(cases)/sizeof(*cases));
    XrXirValue kept=json_float_take(program,selection[2],NULL,0);xr_xir_compile_program_drop(program);return kept;
}
static void json_float_retained(XrXirValue *value) {
    json_float_expected(value,true,UINT64_C(0x7fefffffffffffff));xr_xir_value_drop(value);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
}
#endif // XIR_JSON_FLOAT_RUNTIME_H
