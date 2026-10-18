typedef struct RecursiveLimits { uint64_t steps,frame,output;uint32_t depth; } RecursiveLimits;
static XrXirCtfeStatus recursive_limit(const RecursiveWire *wire,const char *function,bool owning,RecursiveLimits limits) {
    RecursiveJob job;recursive_job_new(&job,wire,function);XrXirValue output={0};
    job.request.evaluation.limits=(XrXirCtfeLimits){limits.steps,limits.frame,limits.output};job.request.call_depth=limits.depth;
    XrCompileResourceStats before=stats(&job.context);XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&job.request,&output);
    if (status==XR_XIR_CTFE_OK) { recursive_value(&job.context,&output,owning);xr_xir_value_drop(&output); }
    else CHECK(!output.type && !output.payload);
    if (limits.output>XR_XIR_CTFE_MAX_OUTPUT_BYTES) CHECK(stats(&job.context).work==before.work && stats(&job.context).allocated_bytes==before.allocated_bytes);
    recursive_job_free(&job);return status;
}
static void recursive_limits(const RecursiveWire *wire,const char *function,bool owning) {
    RecursiveLimits limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES,XR_XIR_CTFE_MAX_CALL_DEPTH};
    uint64_t steps=0;
    for (;;++steps) {
        CHECK(steps<XR_XIR_CTFE_MAX_STEPS);limits.steps=steps;
        XrXirCtfeStatus status=recursive_limit(wire,function,owning,limits);
        if (status==XR_XIR_CTFE_OK) break;CHECK(status==XR_XIR_CTFE_STEP_LIMIT);
    }
    limits.steps=XR_XIR_CTFE_MAX_STEPS;uint32_t depth=1;
    for (;;++depth) {
        CHECK(depth<=XR_XIR_CTFE_MAX_CALL_DEPTH);limits.depth=depth;
        XrXirCtfeStatus status=recursive_limit(wire,function,owning,limits);
        if (status==XR_XIR_CTFE_OK) break;CHECK(status==XR_XIR_CTFE_DEPTH_LIMIT);
    }
    limits.depth=XR_XIR_CTFE_MAX_CALL_DEPTH;uint64_t low=0,high=XR_XIR_CTFE_MAX_FRAME_BYTES;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;limits.frame=middle;
        XrXirCtfeStatus status=recursive_limit(wire,function,owning,limits);
        if (status==XR_XIR_CTFE_OK) high=middle;
        else { CHECK(status==XR_XIR_CTFE_FRAME_LIMIT || status==XR_XIR_CTFE_BUDGET);low=middle+1; }
    }
    CHECK(low);limits.frame=low;CHECK(recursive_limit(wire,function,owning,limits)==XR_XIR_CTFE_OK);
    limits.frame=low-1;XrXirCtfeStatus short_frame=recursive_limit(wire,function,owning,limits);
    CHECK(short_frame==XR_XIR_CTFE_FRAME_LIMIT || short_frame==XR_XIR_CTFE_BUDGET);
    limits.frame=XR_XIR_CTFE_MAX_FRAME_BYTES;limits.output=owning?16:8;
    CHECK(recursive_limit(wire,function,owning,limits)==XR_XIR_CTFE_OK);
    --limits.output;CHECK(recursive_limit(wire,function,owning,limits)==XR_XIR_CTFE_OUTPUT_LIMIT);
    limits.output=XR_XIR_CTFE_MAX_OUTPUT_BYTES+1;CHECK(recursive_limit(wire,function,owning,limits)==XR_XIR_CTFE_BAD_ARGUMENT);
    fprintf(stderr,"Recursive finite request %s owning=%u all steps0..%llu depth1..%u frame%llu exact/minus-one status%u logical output%u exact/minus-one; over8MiB BAD_ARGUMENT zero resource consumption\n",
        function,owning,(unsigned long long)steps,depth,(unsigned long long)low,short_frame,owning?16:8);
}
static void recursive_original_owner(const RecursiveWire *wire,const char *function,bool owning) {
    RecursiveJob job;recursive_job_new(&job,wire,function);XrCompileResources *same=job.context.resources;
    XrXirValue output={0};uint64_t successes=0;
    for (;;) {
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&job.request,&output);
        if (status!=XR_XIR_CTFE_OK) { CHECK(status==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);break; }
        if (owning) CHECK(output.type==(uint32_t)job.lowered->module.functions[job.request.evaluation.function].result && output.payload);
        else CHECK(output.type==XR_XIR_I64 && output.payload==42);
        xr_xir_value_drop(&output);++successes;
    }
    CHECK(successes && job.context.resources==same);
    XrCompileResourceStats spent=stats(&job.context);
    CHECK(spent.work<=caps.work && spent.allocated_bytes<=caps.allocated_bytes && spent.peak_bytes<=caps.live_bytes);
    CHECK(xr_xir_compile_ctfe_calls(&job.request,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);
    fprintf(stderr,"Recursive original owner %s owning=%u successes=%llu allocated=%llu peak=%llu work=%llu/128000000 then same-owner BUDGET retry, empty/live preserved\n",
        function,owning,(unsigned long long)successes,(unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
    recursive_job_free(&job);
}
static void recursive_last_owner(RecursiveWire *wire,const char *function,bool owning) {
    RecursiveJob job;recursive_job_new(&job,wire,function);XrXirValue output={0};
    CHECK(xr_xir_compile_ctfe_calls(&job.request,&output)==XR_XIR_CTFE_OK);
    xr_xir_compile_artifact_free(job.lowered);xr_compile_resources_release(job.context.resources);
    xr_xir_compile_checked_packet_free(&wire->packet);xr_compile_resources_release(wire->producer.resources);
    recursive_value(&job.context,&output,owning);
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_injected=false;source_fixture_compile_fail_at=attempts;
    xr_xir_value_drop(&output);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(!source_fixture_compile_injected && source_fixture_compile_attempts==attempts);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Recursive result %s owning=%u survives Source/packet/Artifact/ledger callers; last drop allocates nothing under next physical failure, physical0\n",function,owning);
}
