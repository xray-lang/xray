/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derived_library_runtime.h - Cold derived library runtime results
 */
#ifndef XIR_SOURCE_DERIVED_LIBRARY_RUNTIME_H
#define XIR_SOURCE_DERIVED_LIBRARY_RUNTIME_H
#include "xir/xxir_vm.h"
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static XrXirValue cold_runtime_call(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,4096).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
static XrXirValue cold_runtime_results(XrXirProgram *program,const uint32_t selection[3]) {
    XrXirValue value=cold_runtime_call(program,selection[1]);CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);
    xr_xir_value_drop(&value);return cold_runtime_call(program,selection[0]);
}
static void cold_runtime_fault_attempt(XrXirProgram *program,uint32_t entry,bool string,bool injected) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;XrXirValue value={0};bool entered_oom=false;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if(status==XR_XIR_CALL_OOM)CHECK(!instance);
    if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
    if(status==XR_XIR_CALL_READY){status=xr_xir_instance_poll_bounded(instance,4096).outcome.status;entered_oom=status==XR_XIR_CALL_OOM;}
    if(status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if(string){const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&value,&bytes,&length) && length==10 && !memcmp(bytes,"left:right",10));}
        else CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);
        xr_xir_value_drop(&value);
    }else if(instance) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);
        CHECK(!value.type && !value.reserved && !value.payload);
    }
    if(instance)CHECK(xr_xir_instance_free(instance)==(entered_oom?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
    if(status!=(injected?XR_XIR_CALL_OOM:XR_XIR_CALL_RETURNED))fprintf(stderr,"Cold runtime entry%u injected%u status%u\n",entry,(unsigned)injected,(unsigned)status);
    CHECK(status==(injected?XR_XIR_CALL_OOM:XR_XIR_CALL_RETURNED));
}
static void cold_runtime_faults(XrXirProgram *program,const uint32_t selection[3],bool faults) {
    size_t live=runtime_live,bytes=runtime_bytes,compiler_live=source_fixture_compile_live,compiler_bytes=source_fixture_compile_bytes;
    for(unsigned input=0;input<2;++input) {
        uint32_t entry=selection[input?0:1];runtime_attempts=0;cold_runtime_fault_attempt(program,entry,input!=0,false);size_t sites=runtime_attempts;
        CHECK(sites && runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
        if(faults)for(size_t ordinal=0;ordinal<sites;++ordinal) {
            runtime_attempts=0;runtime_fail_at=ordinal;cold_runtime_fault_attempt(program,entry,input!=0,true);
            CHECK(runtime_attempts>ordinal && runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
            runtime_fail_at=SIZE_MAX;cold_runtime_fault_attempt(program,entry,input!=0,false);
            CHECK(runtime_live==live && runtime_bytes==bytes && source_fixture_compile_live==compiler_live && source_fixture_compile_bytes==compiler_bytes);
        }
        fprintf(stderr,"Cold library runtime input%u sites%zu full%u same Program retry and both physical baselines\n",input,sites,(unsigned)faults);
    }
}
static void cold_runtime_retained(XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length) && length==10 && !memcmp(bytes,"left:right",10));
    xr_xir_value_drop(value);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
#endif // XIR_SOURCE_DERIVED_LIBRARY_RUNTIME_H
