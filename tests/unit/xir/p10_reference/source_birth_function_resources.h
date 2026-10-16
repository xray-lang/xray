/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_birth_function_resources.h - Physical closure leases at every fault
 */
#ifndef SOURCE_BIRTH_FUNCTION_RESOURCES_H
#define SOURCE_BIRTH_FUNCTION_RESOURCES_H
#include "source_history_function_fixture.h"
static bool birth_function_trial(size_t ordinal,bool publish,bool escaped,BirthCost *cost) {
    CHECK(!runtime_live && !runtime_bytes && !instance_compile_live && !instance_compile_bytes);
    runtime_fail_at=SIZE_MAX;
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirSourceGenerationConfig config={compiler.context,"birth-function-resources",NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;
    CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    XrXirProgram *programs[2]={0};uint32_t entries[2]={0},escape=UINT32_MAX;
    char initial[512];int length=snprintf(initial,sizeof(initial),"%s\nexport fn escape()->fn()->i64{return fetch()}",
        source_history_function_input(1,escaped));
    CHECK(length>0 && (size_t)length<sizeof(initial));
    uint64_t first=birth_source(owner,initial,"seed",&programs[0],&entries[0],&escape);
    XrXirInstanceConfig runtime;CHECK(xr_xir_instance_config_init(&runtime,sizeof(runtime))==XR_XIR_CALL_READY);
    unsigned calls=0;runtime.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,
        .write=birth_output,.context=&calls};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&runtime,&instance)==XR_XIR_CALL_READY);
    uint64_t candidate=0;
    CHECK(xr_xir_source_generation_prepare_instance(owner,first,instance,&candidate)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entries[0],NULL,0)==XR_XIR_CALL_READY &&
        xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0},function={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && !value.payload);
    xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY &&
        xr_xir_compile_source_generation_publish(owner,first)==XR_XIR_OK);
    CHECK(xr_xir_instance_start(instance,escape,NULL,0)==XR_XIR_CALL_READY &&
        xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED &&
        xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(&function);
    CHECK(binding && binding->capture_count==1);
    uint64_t next=birth_source(owner,source_history_function_input(2,escaped),"answer",&programs[1],&entries[1],NULL);
    size_t before_blocks=runtime_live,before_bytes=runtime_bytes;
    runtime_attempts=0;runtime_fail_at=ordinal;candidate=0;
    XrXirCallStatus accepted=XR_XIR_CALL_READY;
    XrXirCallStatus status=xr_xir_source_generation_prepare_instance(owner,next,instance,&candidate);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entries[1],NULL,0);
        if (status==XR_XIR_CALL_READY) {
            status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
            if (status!=XR_XIR_CALL_RETURNED) accepted=status;
            else {
                CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
                if (escaped) {
                    const XrXirFunctionBinding *received=xr_xir_function_binding(&value);
                    CHECK(received && received->owner==binding->owner && received->entry==binding->entry &&
                        received->capture_count==1 && xr_xir_value_arena(&value)==xr_xir_value_arena(&function));
                } else CHECK(value.type==XR_XIR_I64 && value.payload==42);
                status=XR_XIR_CALL_READY;
            }
        }
    }
    bool complete=status==XR_XIR_CALL_READY;
    CHECK(complete || status==XR_XIR_CALL_OOM);
    cost->sites=runtime_attempts;cost->status=status;runtime_fail_at=SIZE_MAX;
    if (complete && publish) {
        CHECK(xr_xir_instance_generation_publish(instance,candidate)==XR_XIR_CALL_READY &&
            xr_xir_compile_source_generation_publish(owner,next)==XR_XIR_OK);
    } else {
        xr_xir_value_drop(&value);
        if (candidate) CHECK(xr_xir_instance_generation_abort(instance,candidate)==accepted);
        CHECK(xr_xir_compile_source_generation_abort(owner,next)==XR_XIR_OK);
        CHECK(runtime_live==before_blocks && runtime_bytes==before_bytes);
    }
    XrXirValue count={0};CHECK(xr_xir_cell_read(&binding->captures[0],&count)==XR_XIR_VALUE_OK);
    CHECK(count.type==XR_XIR_I64 && (count.payload==41 || (!escaped && count.payload==42)));
    if (complete) CHECK(count.payload==(escaped?41:42));
    int64_t expected=count.payload+1;xr_xir_value_drop(&count);
    CHECK(xr_xir_instance_start_function(instance,&function,NULL,0)==XR_XIR_CALL_READY &&
        xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED &&
        xr_xir_instance_take_result(instance,&count)==XR_XIR_CALL_RETURNED);
    CHECK(count.type==XR_XIR_I64 && count.payload==expected);xr_xir_value_drop(&count);
    CHECK(calls==1 && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_source_generations_free(owner);
    for (unsigned i=0;i<2;++i) xr_xir_compile_program_drop(programs[i]);
    CHECK(xr_xir_cell_read(&binding->captures[0],&count)==XR_XIR_VALUE_OK && count.payload==expected);
    xr_xir_value_drop(&count);xr_xir_value_drop(&value);xr_xir_value_drop(&function);
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(compiler.context.resources,&stats)==XR_COMPILE_RESOURCE_OK &&
        stats.live_bytes==compiler.baseline.live_bytes);
    xr_compile_resources_release(compiler.context.resources);
    CHECK(!runtime_live && !runtime_bytes);instance_compile_zero();return complete;
}
#endif // SOURCE_BIRTH_FUNCTION_RESOURCES_H
