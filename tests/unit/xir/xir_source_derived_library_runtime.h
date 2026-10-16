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
static void cold_runtime_retained(XrXirValue *value) {
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(value,&bytes,&length) && length==10 && !memcmp(bytes,"left:right",10));
    xr_xir_value_drop(value);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
#endif // XIR_SOURCE_DERIVED_LIBRARY_RUNTIME_H
