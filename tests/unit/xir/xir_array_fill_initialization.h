/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_fill_initialization.h - Every actual initialization allocation
 *
 * KEY CONCEPT:
 *   Failed initialization stays latched; pre-start allocation refusal is distinct.
 */
#ifndef XIR_ARRAY_FILL_INITIALIZATION_H
#define XIR_ARRAY_FILL_INITIALIZATION_H
static void fill_initialization_faults(FillCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    runtime_attempts=0;CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue result=fill_run(instance,run->program->declarations->entry_function);
    const size_t sites=runtime_attempts;CHECK(result.type==XR_XIR_I64&&!result.payload);xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    size_t latched=0,creation=0,prepared=0;
    for(size_t failure=0;failure<sites;++failure) {
        runtime_attempts=0;runtime_fail_at=failure;instance=NULL;
        XrXirCallStatus status=xr_xir_instance_new(run->program,&config,&instance);
        if(status==XR_XIR_CALL_READY) {
            status=xr_xir_instance_start(instance,run->program->declarations->entry_function,NULL,0);
            while(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
        }
        CHECK(status==XR_XIR_CALL_OOM&&runtime_attempts>failure);runtime_fail_at=SIZE_MAX;
        if(instance) {
            result=(XrXirValue){0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.payload);
            if(instance->state==XR_XIR_INSTANCE_FAILED) {
                XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(instance,&copy)==XR_XIR_CALL_OOM);
                CHECK(copy.status==XR_XIR_CALL_OOM);xr_xir_call_result_drop(&copy);
                CHECK(xr_xir_instance_start(instance,run->program->declarations->entry_function,NULL,0)==XR_XIR_CALL_OOM);++latched;
            } else ++prepared;
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        } else ++creation;
        CHECK(!runtime_live&&!runtime_bytes);
    }
    CHECK(sites && creation && creation + prepared + latched == sites);
#if !defined(XR_FILL_MATRIX)
    CHECK(latched > 0);
#endif
    printf("fill initialization runtimeOOM%zu creation%zu pre-start%zu latched%zu physical0\n",sites,creation,prepared,latched);
}
#endif
