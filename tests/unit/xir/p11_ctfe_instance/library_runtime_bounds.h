/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_runtime_bounds.h - Actual independent Domain counters set exact limits
 */
static void library_native_runtime_bounds(LibraryNativeOwner *owner,XrXirProgram *program,
    size_t runtime_blocks,size_t runtime_size,size_t compiler_blocks,size_t compiler_size) {
    size_t polls=0;LibraryRuntimeMetrics metrics={0};
    CHECK(library_native_runtime_job_limits(owner,program,SIZE_MAX,&polls,NULL,&metrics)==XR_XIR_CALL_RETURNED);
    library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
    /* Call arenas clamp spare segment capacity to the available budget. The
     * default-limit peak is not an admission threshold. Keep actual refused
     * and accepted endpoints; no monotonicity or cached proof is assumed. */
    uint64_t refused=1,accepted=metrics.call_peak;CHECK(accepted>refused);
    XrXirInstanceConfig probe;CHECK(xr_xir_instance_config_init(&probe,sizeof(probe))==XR_XIR_CALL_READY);
    probe.call_limit=refused;size_t probe_polls=0;
    CHECK(library_native_runtime_job_limits(owner,program,SIZE_MAX,&probe_polls,&probe,NULL)==XR_XIR_CALL_LIMIT);
    library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
    probe.call_limit=accepted;
    CHECK(library_native_runtime_job_limits(owner,program,SIZE_MAX,&probe_polls,&probe,NULL)==XR_XIR_CALL_RETURNED);
    library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
    while(accepted-refused>1) {
        uint64_t middle=refused+(accepted-refused)/2;probe.call_limit=middle;
        XrXirCallStatus status=library_native_runtime_job_limits(owner,program,SIZE_MAX,&probe_polls,&probe,NULL);
        CHECK(status==XR_XIR_CALL_LIMIT || status==XR_XIR_CALL_RETURNED);
        if(status==XR_XIR_CALL_RETURNED) accepted=middle;else refused=middle;
        library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
    }
    fprintf(stderr,"Adaptive call arena observed_peak%llu accepted%llu rejected%llu\n",
        (unsigned long long)metrics.call_peak,(unsigned long long)accepted,(unsigned long long)refused);
    uint64_t values[]={metrics.value_peak,metrics.requested_value,metrics.work,metrics.requested_call,metrics.metadata_peak,accepted};
    for(unsigned axis=0;axis<6;++axis) {
        CHECK(values[axis]);
        for(unsigned less=0;less<2;++less) {
            XrXirInstanceConfig limits;CHECK(xr_xir_instance_config_init(&limits,sizeof(limits))==XR_XIR_CALL_READY);
            uint64_t *fields[]={&limits.value_limit,&limits.requested_value_limit,&limits.work_limit,
                &limits.requested_call_limit,&limits.metadata_limit,&limits.call_limit};
            *fields[axis]=values[axis]-less;
            size_t actual=0;XrXirCallStatus status=library_native_runtime_job_limits(owner,program,SIZE_MAX,&actual,&limits,NULL);
            if(status!=(less?XR_XIR_CALL_LIMIT:XR_XIR_CALL_RETURNED))
                fprintf(stderr,"Runtime bound axis%u exact%llu less%u status%u\n",axis,(unsigned long long)values[axis],less,status);
            CHECK(status==(less?XR_XIR_CALL_LIMIT:XR_XIR_CALL_RETURNED));
            library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
            CHECK(library_native_runtime_job(owner,program,SIZE_MAX,&actual)==XR_XIR_CALL_RETURNED && actual==polls);
            library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
        }
    }
    fprintf(stderr,"Runtime six axes exact/minus-one value%llu requested%llu work%llu call-request%llu metadata%llu call-live%llu\n",
        (unsigned long long)values[0],(unsigned long long)values[1],(unsigned long long)values[2],
        (unsigned long long)values[3],(unsigned long long)values[4],(unsigned long long)values[5]);
}
