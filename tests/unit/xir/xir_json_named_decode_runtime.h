/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_named_decode_runtime.h - Name matching and initializer observations
 */
#ifndef XIR_JSON_NAMED_DECODE_RUNTIME_H
#define XIR_JSON_NAMED_DECODE_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
static XrXirValue named_call(XrXirProgram *program,uint32_t entry,const char *text,size_t length) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    XrXirValue argument={0},result={0};CHECK(xr_xir_string_new(domain,text,length,&argument)==XR_XIR_VALUE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,&argument,1)==XR_XIR_CALL_READY);
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static void named_expected(const XrXirValue *value,bool expected,int64_t number) {
    bool some=false;const XrXirValue *payload=NULL;
    CHECK(xr_xir_value_valid(value) && xr_xir_nullable_view(value,&some,&payload) && some==expected);
    if(some)CHECK(payload && payload->type==XR_XIR_I64 && !payload->reserved && payload->payload==number);
    else CHECK(!payload);
}
static void named_case(XrXirProgram *program,uint32_t entry,const char *text,bool some,int64_t expected) {
    XrXirValue result=named_call(program,entry,text,strlen(text));named_expected(&result,some,expected);xr_xir_value_drop(&result);
}
static void named_oracles(XrXirProgram *program,const uint32_t entries[5],const char *left,const char *right) {
    const struct {const char *format;bool some;int64_t value;} cases[]={
        {"{\"%s\":7,\"%s\":9}",true,7009},{"{\"%s\":-7,\"%s\":9}",true,9},
        {"{\"%s\":0,\"%s\":-9}",true,-9},{"{\"%s\":7,\"%s\":9,}",false,0},
        {"{\"%s\":7,\"%s\":9,\"extra\":1}",false,0},{"{\"%s\":7,\"%s\":9.0}",false,0},
        {"{\"%s\":7,\"%s\":null}",false,0},{"{\"%s\":7,\"%s\":true}",false,0},
        {"{\"%s\":7,\"%s\":9223372036854775808}",false,0}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        char text[256];int n=snprintf(text,sizeof(text),cases[i].format,left,right);CHECK(n>0 && n<(int)sizeof(text));
        named_case(program,entries[1],text,cases[i].some,cases[i].value);
    }
    char text[256];int n=snprintf(text,sizeof(text)," \r\n{\"%s\":9,\"%s\":7}\t",right,left);
    CHECK(n>0 && n<(int)sizeof(text));named_case(program,entries[1],text,true,7009);
    n=snprintf(text,sizeof(text),"{\"%s\":7}",left);CHECK(n>0 && n<(int)sizeof(text));named_case(program,entries[1],text,false,0);
    n=snprintf(text,sizeof(text),"{\"%s\":7,\"%s\":9}",left,left);CHECK(n>0 && n<(int)sizeof(text));named_case(program,entries[1],text,false,0);
    named_case(program,entries[1],"{}",false,0);named_case(program,entries[1],"[]",false,0);
    named_case(program,entries[1],"null",false,0);named_case(program,entries[1],"",false,0);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY);XrXirCallStatus status;
    do {status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;}while(status==XR_XIR_CALL_READY);
    XrXirValue result={0};CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    printf("Named JSON initializer:16 independent cases and entry42, actual semantic keys %s/%s\n",left,right);
}
#endif // XIR_JSON_NAMED_DECODE_RUNTIME_H
