/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_persistent_host_faults.h - Real allocator prefix and lifetime limits
 */
#ifndef XIR_PERSISTENT_HOST_FAULTS_H
#define XIR_PERSISTENT_HOST_FAULTS_H
static XrXirCallStatus persistent_host_trial(XrXirProgram *program,uint32_t reader,
    const XrXirInstanceConfig *config,XrXirDomainBudgetStats *stats) {
    XrXirHostInstance *owner=NULL;XrXirHostCall *call=NULL;
    XrXirCallResult function={0},result={0};
    XrXirCallStatus status=xr_xir_host_instance_new(program,config,&owner);
    if(status==XR_XIR_CALL_READY)status=xr_xir_host_instance_execute(owner,reader,NULL,0,&function);
    if(status==XR_XIR_CALL_RETURNED) {
        status=xr_xir_host_instance_begin_function(owner,&function.value,NULL,0,&call);
        if(status==XR_XIR_CALL_READY) {
            status=persistent_until_pause(call).outcome.status;
            if(status==XR_XIR_CALL_RETURNED)status=xr_xir_host_call_take(call,&result);
            if(status==XR_XIR_CALL_RETURNED)CHECK(result.value.type==XR_XIR_I64 && !result.value.payload);
        }
    }
    XrXirCallStatus closed=xr_xir_host_call_drop(call);
    if(status==XR_XIR_CALL_RETURNED && closed!=XR_XIR_CALL_READY)status=closed;
    xr_xir_call_result_drop(&function);xr_xir_call_result_drop(&result);
    if(owner && stats)*stats=xr_xir_domain_budget_stats(owner->instance->domain);
    closed=xr_xir_host_instance_drop(owner);
    if(status==XR_XIR_CALL_RETURNED && closed!=XR_XIR_CALL_READY)status=closed;
    return status;
}
static void persistent_host_faults(XrXirProgram *program,uint32_t reader) {
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirDomainBudgetStats baseline={0};
    for(size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrXirCallStatus status=persistent_host_trial(program,reader,&config,point?NULL:&baseline);
        if(!point) { CHECK(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;CHECK(sites>0); }
        else CHECK(status==XR_XIR_CALL_OOM && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    uint64_t measured[]={baseline.metadata_peak+sizeof(XrXirHostInstance),baseline.call_peak,
        baseline.requested_bytes,baseline.requested_call_bytes,baseline.work};
    /* Frame segments shrink to available aligned capacity. The ordinary peak
     * includes slab headroom, so find the real admitted boundary below it. */
    uint64_t lower=1,upper=measured[1];
    while(lower<upper) {
        uint64_t middle=lower+(upper-lower)/2;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.call_limit=middle;
        XrXirCallStatus status=persistent_host_trial(program,reader,&config,NULL);
        CHECK(status==XR_XIR_CALL_RETURNED || status==XR_XIR_CALL_LIMIT);
        CHECK(runtime_live==live && runtime_bytes==bytes);
        if(status==XR_XIR_CALL_RETURNED)upper=middle;else lower=middle+1;
    }
    measured[1]=lower;
    for(unsigned axis=0;axis<5;++axis)for(int delta=-1;delta<=1;++delta) {
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        uint64_t *limits[]={&config.metadata_limit,&config.call_limit,&config.requested_value_limit,
            &config.requested_call_limit,&config.work_limit};
        CHECK(measured[axis]>1 && measured[axis]<*limits[axis]);
        *limits[axis]=delta<0?measured[axis]-1:measured[axis]+(unsigned)delta;
        XrXirCallStatus status=persistent_host_trial(program,reader,&config,NULL);
        if(status!=(delta<0?XR_XIR_CALL_LIMIT:XR_XIR_CALL_RETURNED))
            fprintf(stderr,"persistent host budget axis=%u delta=%d status=%u measured=%llu\n",
                axis,delta,status,(unsigned long long)measured[axis]);
        CHECK(status==(delta<0?XR_XIR_CALL_LIMIT:XR_XIR_CALL_RETURNED));
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    printf("persistent host allocator prefix=%zu/%zu; metadata=%llu call=%llu (ordinary peak=%llu) value-request=%llu call-request=%llu work=%llu exact boundaries PASS; physical baseline\n",
        sites,sites,(unsigned long long)measured[0],(unsigned long long)measured[1],
        (unsigned long long)baseline.call_peak,(unsigned long long)measured[2],
        (unsigned long long)measured[3],(unsigned long long)measured[4]);
}
#endif // XIR_PERSISTENT_HOST_FAULTS_H
