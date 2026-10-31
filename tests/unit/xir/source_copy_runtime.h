/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_copy_runtime.h - VM and generated C share the independent result and retirement oracle.
 */
#ifndef SOURCE_COPY_RUNTIME_H
#define SOURCE_COPY_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static XrXirValue source_copy_take(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,65536).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return result;
}
static XrXirValue source_copy_run(XrXirProgram *program,const uint32_t entries[2]) {
    size_t live=runtime_live,bytes=runtime_bytes;
    for(unsigned i=0;i<2;++i) {
        XrXirValue answer=source_copy_take(program,entries[0]);
        CHECK(answer.type==XR_XIR_I64 && !answer.reserved && answer.payload==42);xr_xir_value_drop(&answer);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *failed=NULL;runtime_fail_at=runtime_attempts;
    CHECK(xr_xir_instance_new(program,&config,&failed)==XR_XIR_CALL_OOM && !failed);
    runtime_fail_at=SIZE_MAX;CHECK(runtime_live==live && runtime_bytes==bytes);
    XrXirValue kept=source_copy_take(program,entries[1]);xr_xir_compile_program_drop(program);return kept;
}
static void source_copy_retired(XrXirValue *kept) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(kept,&bytes,&length) && length==3 && !memcmp(bytes,"A\0B",3));
    xr_xir_value_drop(kept);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
}
#endif
