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
typedef struct DecodeTrial {const char *text;size_t length;bool some;int64_t number;bool constant;} DecodeTrial;
static bool decode_faults;
static size_t decode_fault_cases,decode_fault_sites;
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
static void decode_trial(XrXirProgram *program,uint32_t entry,const DecodeTrial *trial) {
    XrXirDomain *domain=NULL;XrXirInstance *instance=NULL;XrXirValue argument={0},result={0};
    XrXirCallStatus status=XR_XIR_CALL_READY;bool driven=false;
    if(!trial->constant) {
        XrXirValueStatus made=xr_xir_domain_new(65536,&domain);
        if(made!=XR_XIR_VALUE_OK){CHECK(made==XR_XIR_VALUE_OOM && !domain);status=XR_XIR_CALL_OOM;goto done;}
        made=xr_xir_string_new(domain,trial->text,trial->length,&argument);
        if(made!=XR_XIR_VALUE_OK){CHECK(made==XR_XIR_VALUE_OOM && !argument.type && !argument.reserved && !argument.payload);status=XR_XIR_CALL_OOM;goto done;}
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    status=xr_xir_instance_new(program,&config,&instance);if(status==XR_XIR_CALL_OOM){CHECK(!instance);goto done;}
    CHECK(status==XR_XIR_CALL_READY);
    status=xr_xir_instance_start(instance,entry,trial->constant?NULL:&argument,trial->constant?0:1);
    xr_xir_value_drop(&argument);if(domain){xr_xir_domain_drop(domain);domain=NULL;}
    if(status==XR_XIR_CALL_READY){status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;driven=status==XR_XIR_CALL_OOM;}
    if(status==XR_XIR_CALL_RETURNED)CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    else {
        CHECK(status==XR_XIR_CALL_OOM);XrXirCallStatus expected=xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED || !driven?
            XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_OOM;
        CHECK(xr_xir_instance_take_result(instance,&result)==expected && !result.type && !result.reserved && !result.payload);
    }
done:
    if(instance)CHECK(xr_xir_instance_free(instance)==(driven?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    xr_xir_value_drop(&argument);if(domain)xr_xir_domain_drop(domain);
    CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
    if(status==XR_XIR_CALL_RETURNED) {
        if(trial->constant)CHECK(result.type==XR_XIR_I64 && !result.reserved && result.payload==42);
        else decode_expected(&result,trial->some,trial->number);
    }
    xr_xir_value_drop(&result);
}
static void decode_full_faults(XrXirProgram *program,uint32_t entry,const DecodeTrial *trial) {
    size_t held=runtime_live,bytes=runtime_bytes,compiler=source_fixture_compile_live,compiler_bytes=source_fixture_compile_bytes;
    CHECK(runtime_fail_at==SIZE_MAX);runtime_attempts=0;decode_trial(program,entry,trial);size_t sites=runtime_attempts;CHECK(sites);
    CHECK(runtime_live==held && runtime_bytes==bytes && source_fixture_compile_live==compiler && source_fixture_compile_bytes==compiler_bytes);
    for(size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;decode_trial(program,entry,trial);CHECK(runtime_attempts>site);
        CHECK(runtime_live==held && runtime_bytes==bytes && source_fixture_compile_live==compiler && source_fixture_compile_bytes==compiler_bytes);
        runtime_fail_at=SIZE_MAX;decode_trial(program,entry,trial);
        CHECK(runtime_live==held && runtime_bytes==bytes && source_fixture_compile_live==compiler && source_fixture_compile_bytes==compiler_bytes);
    }
    ++decode_fault_cases;decode_fault_sites+=sites;
}
static void decode_case(XrXirProgram *program,uint32_t entry,const char *text,size_t length,bool some,int64_t number) {
    if(decode_faults){DecodeTrial trial={text,length,some,number,false};decode_full_faults(program,entry,&trial);return;}
    XrXirValue value=decode_call(program,entry,text,length);decode_expected(&value,some,number);xr_xir_value_drop(&value);
}
static void decode_oracles(XrXirProgram *program,const uint32_t entries[5]) {
    uint32_t main=entries[0],decode=entries[1];
    const struct {const char *text;bool some;int64_t number;} cases[]={
        {"0",true,0},{"-0",true,0},{"42",true,42},{"-7",true,0},{"9223372036854775807",true,INT64_MAX},
        {"-9223372036854775808",true,0},{"9223372036854775808",false,0},{"-9223372036854775809",false,0},
        {"0.0",false,0},{"1e0",false,0},{"01",false,0},{"-01",false,0},{"+1",false,0},{"",false,0},
        {"null",false,0},{"true",false,0},{" 1",false,0},{"1 ",false,0},{"\"1\"",false,0}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        decode_case(program,decode,cases[i].text,strlen(cases[i].text),cases[i].some,cases[i].number);
    }
    const char nul[]={'1',0,'2'};decode_case(program,decode,nul,sizeof(nul),false,0);
    if(decode_faults){DecodeTrial trial={NULL,0,false,0,true};decode_full_faults(program,main,&trial);
        printf("Derived JSON decoder full runtime FI: cumulative cases=%zu actual allocation sites=%zu, every ordinal and same Program retry\n",decode_fault_cases,decode_fault_sites);}
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,main,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    puts("Ordinary derived JSON integer decoder:20 independent cases, exact init clamp/None/comptime42, input Domain death");
}
#endif // XIR_SOURCE_DERIVATION_JSON_DECODE_RUNTIME_H
