/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_derivation_runtime.h - Real generated method runtime ownership
 */
#ifndef XIR_SOURCE_DERIVATION_RUNTIME_H
#define XIR_SOURCE_DERIVATION_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
static void derive_runtime_attempt(XrXirProgram *program,uint32_t entry) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;bool driven=false;XrXirValue value={0};
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_OOM) CHECK(!instance);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
    if (status==XR_XIR_CALL_READY) {status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;driven=status==XR_XIR_CALL_OOM;}
    CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
    if (status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_I64 && !value.reserved && value.payload==42);xr_xir_value_drop(&value);
    } else if (instance) {
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);
        CHECK(!value.type && !value.reserved && !value.payload);
    }
    if (instance) CHECK(xr_xir_instance_free(instance)==(driven?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
}
static size_t derive_runtime_failures(XrXirProgram *program,uint32_t entry,bool faults) {
    size_t live=runtime_live,bytes=runtime_bytes;runtime_attempts=0;
    derive_runtime_attempt(program,entry);size_t sites=runtime_attempts;
    CHECK(sites && runtime_live==live && runtime_bytes==bytes);
    if (faults) for (size_t i=0;i<sites;++i) {
        runtime_attempts=0;runtime_fail_at=i;derive_runtime_attempt(program,entry);
        CHECK(runtime_attempts>i && runtime_live==live && runtime_bytes==bytes);
        runtime_fail_at=SIZE_MAX;derive_runtime_attempt(program,entry);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    return sites;
}
#endif // XIR_SOURCE_DERIVATION_RUNTIME_H
