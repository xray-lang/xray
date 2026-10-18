/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_json_aggregate_encode_runtime.h - Independent aggregate output oracles
 */
#ifndef XIR_JSON_AGGREGATE_ENCODE_RUNTIME_H
#define XIR_JSON_AGGREGATE_ENCODE_RUNTIME_H
#include "xir/xxir_vm.h"
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static inline XrXirCompileContext aggregate_runtime_context(void) {
    XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();return context;
}
static void aggregate_expect(const XrXirValue *value,unsigned input) {
    CHECK(input<7);
    if(input==6){CHECK(value->type==XR_XIR_I64 && !value->reserved && value->payload==42);return;}
    const char *expected[]={"{\"values\":[],\"optional\":null}","{\"values\":[1,-2,42],\"optional\":7}",
        "{\"values\":[-9223372036854775808,9223372036854775807],\"optional\":-9223372036854775808}",
        "{\"values\":[0,0,0],\"optional\":0}","{\"values\":[7],\"optional\":null}",NULL};
    char longer[2048];size_t used=(size_t)snprintf(longer,sizeof(longer),"{\"values\":[");
    for(int i=0;i<64;++i){int n=snprintf(longer+used,sizeof(longer)-used,"%s%d",i?",":"",i-32);
        CHECK(n>0 && (size_t)n<sizeof(longer)-used);used+=(size_t)n;}
    CHECK(snprintf(longer+used,sizeof(longer)-used,"],\"optional\":9223372036854775807}")>0);expected[5]=longer;
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(expected[input]) && !memcmp(bytes,expected[input],length));
}
static XrXirValue aggregate_call(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,16384).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
static void aggregate_runtime_results(XrXirProgram *program,const uint32_t selection[10],XrXirValue values[6]) {
    for(unsigned i=0;i<6;++i)values[i]=aggregate_call(program,selection[i]);
    XrXirValue answer=aggregate_call(program,selection[6]);aggregate_expect(&answer,6);xr_xir_value_drop(&answer);
}
static void aggregate_retained(XrXirValue values[6]) {
    for(unsigned i=0;i<6;++i){aggregate_expect(&values[i],i);xr_xir_value_drop(&values[i]);}
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void aggregate_fault_attempt(XrXirProgram *program,uint32_t entry,unsigned input,bool injected) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;XrXirValue value={0};bool entered_oom=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if(status==XR_XIR_CALL_OOM)CHECK(!instance);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status==XR_XIR_CALL_READY){status=xr_xir_instance_poll_bounded(instance,16384).outcome.status;entered_oom=status==XR_XIR_CALL_OOM;}
    if(status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);aggregate_expect(&value,input);xr_xir_value_drop(&value);
    }else if(instance) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);
        CHECK(!value.type && !value.reserved && !value.payload);
    }
    if(instance)CHECK(xr_xir_instance_free(instance)==(entered_oom?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    if(status!=(injected?XR_XIR_CALL_OOM:XR_XIR_CALL_RETURNED))fprintf(stderr,"Aggregate runtime input%u injected%u status%u\n",input,(unsigned)injected,(unsigned)status);
    CHECK(status==(injected?XR_XIR_CALL_OOM:XR_XIR_CALL_RETURNED));
}
static inline void aggregate_runtime_faults(XrXirProgram *program,const uint32_t selection[10],bool faults) {
    size_t live=runtime_live,bytes=runtime_bytes,compiler_live=source_fixture_compile_live,compiler_bytes=source_fixture_compile_bytes;
    for(unsigned input=0;input<7;++input) {
        runtime_attempts=0;aggregate_fault_attempt(program,selection[input],input,false);size_t sites=runtime_attempts;CHECK(sites);
        CHECK(runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
        if(faults)for(size_t ordinal=0;ordinal<sites;++ordinal) {
            runtime_attempts=0;runtime_fail_at=ordinal;aggregate_fault_attempt(program,selection[input],input,true);
            CHECK(runtime_attempts>ordinal && runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
            runtime_fail_at=SIZE_MAX;aggregate_fault_attempt(program,selection[input],input,false);
            CHECK(runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
        }
        fprintf(stderr,"Aggregate runtime input%u sites%zu full%u same Program retry and both physical baselines\n",input,sites,(unsigned)faults);
    }
}
#endif // XIR_JSON_AGGREGATE_ENCODE_RUNTIME_H
