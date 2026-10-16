/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_json_decode_runtime.h - Independent decoded init cases
 */
#ifndef XIR_SOURCE_DERIVATION_JSON_DECODE_RUNTIME_H
#define XIR_SOURCE_DERIVATION_JSON_DECODE_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
static XrXirValue decode_call(XrXirProgram *program,uint32_t entry,const char *text,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,text,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void decode_expected(const XrXirValue *value,bool expected,int64_t number) {
    bool some=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(value,&some,&payload) && some==expected);
    if(some)CHECK(payload && payload->type==XR_XIR_I64 && !payload->reserved && payload->payload==number);else CHECK(!payload);
}
static void decode_oracles(XrXirProgram *program,const uint32_t entries[5]) {
    uint32_t main=entries[0],decode=entries[1];
    const struct {const char *text;bool some;int64_t number;} cases[]={
        {"0",true,0},{"-0",true,0},{"42",true,42},{"-7",true,0},{"9223372036854775807",true,INT64_MAX},
        {"-9223372036854775808",true,0},{"9223372036854775808",false,0},{"-9223372036854775809",false,0},
        {"0.0",false,0},{"1e0",false,0},{"01",false,0},{"-01",false,0},{"+1",false,0},{"",false,0},
        {"null",false,0},{"true",false,0},{" 1",false,0},{"1 ",false,0},{"\"1\"",false,0}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        XrXirValue value=decode_call(program,decode,cases[i].text,strlen(cases[i].text));
        decode_expected(&value,cases[i].some,cases[i].number);xr_xir_value_drop(&value);
    }
    const char nul[]={'1',0,'2'};XrXirValue value=decode_call(program,decode,nul,sizeof(nul));decode_expected(&value,false,0);xr_xir_value_drop(&value);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,main,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    puts("Ordinary derived JSON integer decoder:20 independent cases, exact init clamp/None/comptime42, input Domain death");
}
#endif // XIR_SOURCE_DERIVATION_JSON_DECODE_RUNTIME_H
