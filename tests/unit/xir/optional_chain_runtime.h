/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * optional_chain_runtime.h - Actual execution, cancellation and physical retirement
 *
 * KEY CONCEPT:
 *   Both backends retain the returned managed value after its execution owners die.
 */
#ifndef OPTIONAL_CHAIN_RUNTIME_H
#define OPTIONAL_CHAIN_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_nullable.h"
static XrXirCallStatus optional_drive(XrXirInstance *instance) {
    XrXirInstanceResult result;unsigned ticks=0;
    do {CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,1024);}
    while(result.outcome.status==XR_XIR_CALL_READY);
    return result.outcome.status;
}
static void optional_run(XrXirProgram *program,const uint32_t ids[2]) {
    XrXirInstanceConfig config;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    uint32_t entry=program->declarations->entry_function;XrXirValue result={0};
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(optional_drive(instance)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&result);
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
        CHECK(optional_drive(instance)==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        CHECK(result.type==XR_XIR_I64 && result.payload==42);xr_xir_value_drop(&result);
    }
    CHECK(xr_xir_instance_start(instance,ids[0],NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(optional_drive(instance)==XR_XIR_CALL_CANCELLED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(optional_drive(instance)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
    CHECK(optional_drive(instance)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    bool some=false;XrXirValue text={0};const XrXirValue *borrowed=NULL;
    CHECK(xr_xir_nullable_view(&result,&some,&borrowed) && some && borrowed);
    CHECK(xr_xir_value_copy(borrowed,&text)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&result);const char *bytes=NULL;size_t count=0;
    CHECK(xr_xir_string_view(&text,&bytes,&count) && count==3 && !memcmp(bytes,"A\0B",3));
    xr_xir_value_drop(&text);CHECK(!runtime_live && !runtime_bytes);
}
#endif
