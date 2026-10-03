/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_gap_runtime.h - Ordered defaults with owned lifetime and OOM gates
 *
 * KEY CONCEPT:
 *   Each consumer has fixed expected results; failures restore physical ownership.
 */
#ifndef XIR_DEFAULT_GAP_RUNTIME_H
#define XIR_DEFAULT_GAP_RUNTIME_H
static bool gap_pair(XrXirProgram *program,const uint32_t ids[3],XrXirValue held[2]) {
    for(uint32_t n=0;n<2;++n){
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        for(uint32_t e=0;e<3 && status!=XR_XIR_CALL_OOM;++e){
            CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_RETURNED);
            status=xr_xir_instance_start(instance,ids[e],NULL,0);
            if(status==XR_XIR_CALL_READY){
                XrXirInstanceResult run=xr_xir_instance_poll_bounded(instance, UINT64_MAX);uint32_t yields=0;
                while(run.outcome.status==XR_XIR_CALL_SUSPENDED){
                    CHECK(e==2 && ++yields==1);
                    status=xr_xir_instance_resume(instance,run.epoch,run.outcome.wake);
                    if(status!=XR_XIR_CALL_READY)break;
                    run=xr_xir_instance_poll_bounded(instance, UINT64_MAX);
                }
                if(status==XR_XIR_CALL_READY)status=run.outcome.status;
                if(status==XR_XIR_CALL_RETURNED)CHECK(yields==(e==2 ? 1u : 0u));
            }
            if(status==XR_XIR_CALL_RETURNED){
                XrXirValue value={0};status=xr_xir_instance_take_result(instance,&value);
                if(status==XR_XIR_CALL_RETURNED){
                    if(e==1)held[n]=value;
                    else {CHECK(value.type==XR_XIR_I64 && value.payload==(e==0 ? 1234u : 41u));xr_xir_value_drop(&value);}
                }
            }
            CHECK(status==XR_XIR_CALL_RETURNED || status==XR_XIR_CALL_OOM);
        }
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(status==XR_XIR_CALL_OOM)return false;
    }
    return true;
}
static void gap_drop_pair(XrXirValue held[2]) {
    for(uint32_t i=0;i<2;++i)xr_xir_value_drop(&held[i]);
}
static void gap_runtime_faults(XrXirProgram *program,const uint32_t ids[3]) {
    size_t base=runtime_live,bytes=runtime_bytes,sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        XrXirValue held[2]={{0},{0}};runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        bool success=gap_pair(program,ids,held);
        if(!pass){CHECK(success);sites=runtime_attempts;CHECK(sites);}
        else CHECK(!success);
        gap_drop_pair(held);CHECK(runtime_live==base && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;printf("default gaps runtime OOM=%zu physical baseline restored\n",sites);
}
static void gap_seal_faults(const XrXirProgramSpec *spec) {
    size_t base=runtime_live,bytes=runtime_bytes,sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        XrXirProgram *program=NULL;runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        XrXirStatus status=xr_xir_program_seal(spec,(XrXirProgramBudget){33554432,64000000},&program);
        if(!pass){CHECK(status==XR_XIR_OK && program);sites=runtime_attempts;CHECK(sites);xr_xir_program_drop(program);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && !program);
        CHECK(runtime_live==base && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;printf("default gaps seal OOM=%zu physical baseline restored\n",sites);
}
static void gap_retained(XrXirValue held[2]) {
    runtime_attempts=0;runtime_fail_at=0;
    for(uint32_t n=0;n<2;++n){
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&held[n],&bytes,&length));
        CHECK(length==13 && !memcmp(bytes,"default-owned",13));xr_xir_value_drop(&held[n]);
    }
    CHECK(!runtime_attempts && !runtime_live && !runtime_bytes);runtime_fail_at=SIZE_MAX;
}
#endif /* XIR_DEFAULT_GAP_RUNTIME_H */
