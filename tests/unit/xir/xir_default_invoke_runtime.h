/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_default_invoke_runtime.h - Purpose invokes preserve local error ownership
 *
 * KEY CONCEPT:
 *   Catching, cleanup and failure never bypass declaration-owned defaults.
 */
#ifndef XIR_DEFAULT_INVOKE_RUNTIME_H
#define XIR_DEFAULT_INVOKE_RUNTIME_H
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
static bool default_invoke_pair(XrXirProgram *program,const uint32_t ids[14],XrXirValue held[2][3]) {
    for(uint32_t n=0;n<2;++n){
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        for(uint32_t e=0;e<14 && status!=XR_XIR_CALL_OOM;++e){
            status=xr_xir_instance_start(instance,ids[e],NULL,0);
            if(status==XR_XIR_CALL_READY){
                XrXirInstanceResult run=xr_xir_instance_poll_bounded(instance, UINT64_MAX);uint32_t yields=0;
                while(run.outcome.status==XR_XIR_CALL_SUSPENDED){
                    CHECK(e==13 && ++yields==1);
                    status=xr_xir_instance_resume(instance,run.epoch,run.outcome.wake);
                    if(status!=XR_XIR_CALL_READY)break;
                    run=xr_xir_instance_poll_bounded(instance, UINT64_MAX);
                }
                if(status==XR_XIR_CALL_READY)status=run.outcome.status;
                if(status!=XR_XIR_CALL_OOM)CHECK(yields==(e==13 ? 1u : 0u));
            }
            if(status==XR_XIR_CALL_OOM)break;
            CHECK(status==(e==12 ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
            XrXirValue value={0};status=xr_xir_instance_take_result(instance,&value);
            if(status==XR_XIR_CALL_OOM)break;
            CHECK(status==(e==12 ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
            if(e>=10 && e<=12)held[n][e-10]=value;
            else {CHECK(value.type==XR_XIR_I64 && value.payload==41);xr_xir_value_drop(&value);}
        }
        if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(status==XR_XIR_CALL_OOM)return false;
    }
    return true;
}
static void default_invoke_drop_pair(XrXirValue held[2][3]) {
    for(uint32_t i=0;i<2;++i)for(uint32_t e=0;e<3;++e)xr_xir_value_drop(&held[i][e]);
}
static void default_invoke_runtime_faults(XrXirProgram *program,const uint32_t ids[14]) {
    size_t base=runtime_live,bytes=runtime_bytes,sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        XrXirValue held[2][3]={0};runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
        bool success=default_invoke_pair(program,ids,held);
        if(!pass){CHECK(success);sites=runtime_attempts;CHECK(sites);}
        else CHECK(!success);
        default_invoke_drop_pair(held);CHECK(runtime_live==base && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;printf("default invoke runtime OOM=%zu physical baseline restored\n",sites);
}
static void default_invoke_seal_faults(DefaultBuild build) {
    default_compiler_faults(build,0);
}

static void default_invoke_cancel(XrXirProgram *program,uint32_t entry) {
    size_t base=runtime_live,bytes=runtime_bytes;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult run=xr_xir_instance_poll_bounded(instance, UINT64_MAX);CHECK(run.outcome.status==XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE && !result.type);
    CHECK(xr_xir_instance_resume(instance,run.epoch,run.outcome.wake)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==base && runtime_bytes==bytes);
}
static void default_invoke_read_faults(const XrXirValue *error) {
    size_t base=runtime_live,bytes=runtime_bytes,sites=0;XrXirValue borrowed={0};
    CHECK(xr_xir_error_borrow(error,&borrowed));
    for(size_t pass=0;pass<=sites;++pass){
        runtime_attempts=0;runtime_fail_at=pass ? pass-1 : SIZE_MAX;
        XrXirDomain *domain=NULL;XrXirValue message={0};
        XrXirValueStatus status=xr_xir_domain_new(65536,&domain);
        if(status==XR_XIR_VALUE_OK){
            XrXirValueAdmission admission={xr_xir_value_arena(error),domain,NULL,NULL,10000,65536};
            status=xr_xir_enum_get(&borrowed,0,0,&admission,&message);
        }
        if(!pass){CHECK(status==XR_XIR_VALUE_OK);sites=runtime_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_VALUE_OOM && !message.type);
        xr_xir_value_drop(&message);xr_xir_domain_drop(domain);
        CHECK(runtime_live==base && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;printf("default invoke retained-reader OOM=%zu physical baseline restored\n",sites);
}
static void default_invoke_retained(XrXirValue held[2][3]) {
    default_invoke_read_faults(&held[0][1]);
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    XrXirDomain *read_domain=NULL;CHECK(xr_xir_domain_new(65536,&read_domain)==XR_XIR_VALUE_OK);
    uint64_t read_baseline=xr_xir_domain_stats(read_domain).live_bytes;
    for(uint32_t n=0;n<2;++n){
        const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&held[n][0],&bytes,&length));
        CHECK(length==12 && !memcmp(bytes,"caught-owned",12));xr_xir_value_drop(&held[n][0]);
        for(uint32_t e=1;e<3;++e){
            XrXirValue borrowed={0},message={0};
            if(e==1){CHECK(held[n][e].type==XR_XIR_ERROR);CHECK(xr_xir_error_borrow(&held[n][e],&borrowed));}
            else {CHECK(held[n][e].type!=XR_XIR_ERROR);borrowed=held[n][e];uint32_t variant=UINT32_MAX;
                CHECK(xr_xir_enum_variant(&borrowed,&variant)==XR_XIR_VALUE_OK && variant==0);}
            XrXirValueAdmission admission={xr_xir_value_arena(&held[n][e]),read_domain,NULL,NULL,10000,65536};
            XrXirValueAdmission limited=admission;limited.work=0;
            CHECK(xr_xir_enum_get(&borrowed,0,0,&limited,&message)==XR_XIR_VALUE_LIMIT && !message.type);
            limited=admission;limited.scratch_bytes=0;
            CHECK(xr_xir_enum_get(&borrowed,0,0,&limited,&message)==XR_XIR_VALUE_LIMIT && !message.type);
            XrXirValueStatus got=xr_xir_enum_get(&borrowed,0,0,&admission,&message);
            if(got!=XR_XIR_VALUE_OK)fprintf(stderr,"retained n=%u e=%u type=%u status=%u arena=%p work=%llu\n",n,e,borrowed.type,got,(const void *)admission.arena,(unsigned long long)admission.work);
            CHECK(got==XR_XIR_VALUE_OK);
            size_t allocations=runtime_attempts;xr_xir_value_drop(&held[n][e]);CHECK(runtime_attempts==allocations);
            CHECK(xr_xir_string_view(&message,&bytes,&length));CHECK(length==10 && !memcmp(bytes,"held-error",10));
            xr_xir_value_drop(&message);
        }
    }
    CHECK(xr_xir_domain_stats(read_domain).live_bytes==read_baseline);xr_xir_domain_drop(read_domain);
    CHECK(!runtime_live && !runtime_bytes);runtime_fail_at=SIZE_MAX;
}
#endif /* XIR_DEFAULT_INVOKE_RUNTIME_H */
