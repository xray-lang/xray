static void recursive_wire_free(RecursiveWire *wire) {
    xr_xir_compile_checked_packet_free(&wire->packet);xr_compile_resources_release(wire->producer.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static size_t recursive_source_census(const RecursiveInput *input,XrCompileResourceStats *used) {
    XrXirCompileContext context;context_new(&context);XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
    size_t start=source_fixture_compile_attempts;XrXirStatus status=recursive_source(&context,input,&output,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Recursive Source census %s status=%u %d:%d %s\n",input->name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && output.checked && output.snapshot);size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    *used=stats(&context);xr_xir_compile_source_result_free(&output);xr_compile_resources_release(context.resources);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Recursive Source %s census=%zu allocated=%llu peak=%llu work=%llu\n",input->name,sites,
        (unsigned long long)used->allocated_bytes,(unsigned long long)used->peak_bytes,(unsigned long long)used->work);
    return sites;
}
static bool recursive_retry_budget(const XrCompileResourceStats *failed,const XrCompileResourceStats *need) {
    CHECK(need->work<=caps.work && need->allocated_bytes<=caps.allocated_bytes);
    return failed->work>caps.work-need->work || failed->allocated_bytes>caps.allocated_bytes-need->allocated_bytes;
}
static void recursive_source_faults(const RecursiveInput *input,size_t sites,bool ends,const XrCompileResourceStats *used) {
    size_t retry_ok=0,retry_budget=0;
    for (size_t fault=0;fault<sites;++fault) {
        if (ends && fault && fault+1<sites) continue;
        XrXirCompileContext context;context_new(&context);XrCompileResources *same=context.resources;
        XrCompileResourceStats baseline=stats(&context);size_t start=source_fixture_compile_attempts;
        source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=recursive_source(&context,input,&output,&diagnostic);
        if (status!=XR_XIR_OUT_OF_MEMORY || source_fixture_compile_attempts!=start+fault+1)
            fprintf(stderr,"Recursive Source FI mismatch %s ordinal=%zu status=%u attempts=%zu expected=%zu injected=%u\n",input->name,fault,status,source_fixture_compile_attempts-start,fault+1,source_fixture_compile_injected);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected && !output.checked && !output.snapshot);
        CHECK(source_fixture_compile_attempts==start+fault+1 && stats(&context).live_bytes==baseline.live_bytes);
        XrCompileResourceStats failed=stats(&context),need={.allocated_bytes=used->allocated_bytes-baseline.allocated_bytes,.work=used->work-baseline.work};
        source_fixture_compile_fail_at=SIZE_MAX;bool exhausted=recursive_retry_budget(&failed,&need);
        status=recursive_source(&context,input,&output,&diagnostic);
        if (status!=(exhausted?XR_XIR_BUDGET:XR_XIR_OK)) fprintf(stderr,"Recursive Source retry %s ordinal=%zu expected_budget=%u status=%u prior_work=%llu need=%llu %s\n",
            input->name,fault,exhausted,status,(unsigned long long)failed.work,(unsigned long long)need.work,diagnostic.message);
        CHECK(status==(exhausted?XR_XIR_BUDGET:XR_XIR_OK));
        if (exhausted) {
            CHECK(!output.checked && !output.snapshot && stats(&context).live_bytes==baseline.live_bytes);
            CHECK(recursive_source(&context,input,&output,&diagnostic)==XR_XIR_BUDGET && !output.checked && !output.snapshot);++retry_budget;
        } else {
            CHECK(output.checked && output.snapshot);xr_xir_compile_source_result_free(&output);
            CHECK(stats(&context).work-failed.work==need.work && stats(&context).allocated_bytes-failed.allocated_bytes==need.allocated_bytes);++retry_ok;
        }
        CHECK(context.resources==same && stats(&context).live_bytes==baseline.live_bytes);
        CHECK(stats(&context).work>=failed.work && stats(&context).allocated_bytes>=failed.allocated_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
        if (!(fault%128) || fault+1==sites) fprintf(stderr,"Recursive Source %s FI=%zu/%zu same-owner retry/live physical0\n",input->name,fault+1,sites);
    }
    fprintf(stderr,"Recursive Source %s %s FI terminal actual=%zu retry_ok=%zu predicted_budget_retry=%zu; original128M/64MiB/8MiB unchanged\n",
        input->name,ends?"FIRST_LAST_ONLY":"COMPLETE",sites,retry_ok,retry_budget);
}
static void recursive_source_bounds(const RecursiveInput *input,const XrCompileResourceStats *used) {
    for (unsigned axis=0;axis<3;++axis) for (unsigned shortfall=0;shortfall<2;++shortfall) {
        XrCompileResourceLimits limits=caps;
        if (!axis) limits.allocated_bytes=used->allocated_bytes-shortfall;
        if (axis==1) limits.live_bytes=used->peak_bytes-shortfall;
        if (axis==2) limits.work=used->work-shortfall;
        XrXirCompileContext context={0};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();XrCompileResources *same=context.resources;XrCompileResourceStats baseline=stats(&context);
        XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=recursive_source(&context,input,&output,&diagnostic);
        if (status!=(shortfall?XR_XIR_BUDGET:XR_XIR_OK)) fprintf(stderr,"Recursive Source %s axis=%u short=%u status=%u %s\n",input->name,axis,shortfall,status,diagnostic.message);
        CHECK(status==(shortfall?XR_XIR_BUDGET:XR_XIR_OK));
        if (!shortfall) { CHECK(output.checked && output.snapshot);xr_xir_compile_source_result_free(&output); }
        else {
            CHECK(!output.checked && !output.snapshot && stats(&context).live_bytes==baseline.live_bytes);
            CHECK(recursive_source(&context,input,&output,&diagnostic)==XR_XIR_BUDGET && !output.checked && !output.snapshot);
        }
        CHECK(context.resources==same && stats(&context).live_bytes==baseline.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    fprintf(stderr,"Recursive Source %s three original axes exact/minus-one same-owner retry physical0\n",input->name);
}
static void recursive_packet_faults(const RecursiveInput *input,bool full,bool ends) {
    RecursiveWire wire;recursive_wire_new(&wire,input);size_t blocks=source_fixture_compile_live,bytes=source_fixture_compile_bytes;
    XrXirCompileContext context;context_new(&context);XrXirArtifact *lowered=NULL;size_t start=source_fixture_compile_attempts;
    CHECK(recursive_pipeline(&context,&wire.packet,&lowered)==XR_XIR_OK);size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);
    CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
    fprintf(stderr,"Recursive packet %s census=%zu\n",input->name,sites);
    if (full || ends) for (size_t fault=0;fault<sites;++fault) {
        if (ends && fault && fault+1<sites) continue;
        context_new(&context);XrCompileResources *same=context.resources;XrCompileResourceStats baseline=stats(&context);lowered=NULL;
        start=source_fixture_compile_attempts;source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
        XrXirStatus status=recursive_pipeline(&context,&wire.packet,&lowered);
        if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"Recursive packet FI ordinal=%zu status=%u\n",fault,status);
        CHECK(status==XR_XIR_OUT_OF_MEMORY && !lowered && source_fixture_compile_injected);
        CHECK(source_fixture_compile_attempts==start+fault+1 && stats(&context).live_bytes==baseline.live_bytes);
        XrCompileResourceStats failed=stats(&context);source_fixture_compile_fail_at=SIZE_MAX;
        CHECK(recursive_pipeline(&context,&wire.packet,&lowered)==XR_XIR_OK && lowered);xr_xir_compile_artifact_free(lowered);
        CHECK(context.resources==same && stats(&context).live_bytes==baseline.live_bytes && stats(&context).work>=failed.work);
        xr_compile_resources_release(context.resources);CHECK(source_fixture_compile_live==blocks && source_fixture_compile_bytes==bytes);
        if (!(fault%32) || fault+1==sites) fprintf(stderr,"Recursive packet %s FI=%zu/%zu same-owner retry/live\n",input->name,fault+1,sites);
    }
    context_new(&context);lowered=NULL;CHECK(recursive_pipeline(&context,&wire.packet,&lowered)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&wire.packet);xr_compile_resources_release(wire.producer.resources);xr_compile_resources_release(context.resources);
    recursive_execute(&lowered);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Recursive packet %s %s actual=%zu independent Source/packet/producer dead two Instance42 physical0\n",input->name,full?"COMPLETE_FI":ends?"FIRST_LAST_ONLY":"CENSUS_ONLY",sites);
}
static size_t recursive_evaluator_census(const RecursiveWire *wire,const char *function,bool owning,XrCompileResourceStats *need) {
    RecursiveJob job;recursive_job_new(&job,wire,function);XrXirValue output={0};size_t start=source_fixture_compile_attempts;
    CHECK(xr_xir_compile_ctfe_calls(&job.request,&output)==XR_XIR_CTFE_OK);size_t sites=source_fixture_compile_attempts-start;CHECK(sites);
    XrCompileResourceStats used=stats(&job.context);*need=(XrCompileResourceStats){.allocated_bytes=used.allocated_bytes-job.baseline.allocated_bytes,.work=used.work-job.baseline.work};
    fprintf(stderr,"Recursive evaluator %s owning=%u census=%zu work=%llu\n",function,owning,sites,(unsigned long long)stats(&job.context).work);
    recursive_value(&job.context,&output,owning);xr_xir_value_drop(&output);recursive_job_free(&job);return sites;
}
static void recursive_evaluator_faults(const RecursiveWire *wire,const char *function,bool owning,size_t sites,bool ends,const XrCompileResourceStats *need) {
    size_t retry_ok=0,retry_budget=0;
    for (size_t fault=0;fault<sites;++fault) {
        if (ends && fault && fault+1<sites) continue;
        RecursiveJob job;recursive_job_new(&job,wire,function);XrCompileResources *same=job.context.resources;XrXirValue output={0};
        size_t start=source_fixture_compile_attempts;source_fixture_compile_injected=false;source_fixture_compile_fail_at=start+fault;
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&job.request,&output);
        if (status!=XR_XIR_CTFE_OUT_OF_MEMORY || source_fixture_compile_attempts!=start+fault+1)
            fprintf(stderr,"Recursive evaluator FI %s ordinal=%zu status=%u attempts=%zu expected=%zu injected=%u\n",function,fault,status,source_fixture_compile_attempts-start,fault+1,source_fixture_compile_injected);
        CHECK(status==XR_XIR_CTFE_OUT_OF_MEMORY && source_fixture_compile_injected && !output.type && !output.payload);
        CHECK(source_fixture_compile_attempts==start+fault+1 && stats(&job.context).live_bytes==job.baseline.live_bytes);
        XrCompileResourceStats failed=stats(&job.context);source_fixture_compile_fail_at=SIZE_MAX;start=source_fixture_compile_attempts;
        bool exhausted=recursive_retry_budget(&failed,need);status=xr_xir_compile_ctfe_calls(&job.request,&output);
        if (status!=(exhausted?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK)) fprintf(stderr,"Recursive evaluator retry %s ordinal=%zu expected_budget=%u status=%u prior_work=%llu need=%llu\n",
            function,fault,exhausted,status,(unsigned long long)failed.work,(unsigned long long)need->work);
        CHECK(status==(exhausted?XR_XIR_CTFE_BUDGET:XR_XIR_CTFE_OK));
        if (exhausted) {
            CHECK(!output.type && !output.payload && stats(&job.context).live_bytes==job.baseline.live_bytes);
            CHECK(xr_xir_compile_ctfe_calls(&job.request,&output)==XR_XIR_CTFE_BUDGET && !output.type && !output.payload);++retry_budget;
        } else {
            CHECK(source_fixture_compile_attempts-start==sites && stats(&job.context).work-failed.work==need->work);
            CHECK(stats(&job.context).allocated_bytes-failed.allocated_bytes==need->allocated_bytes);
            recursive_value(&job.context,&output,owning);xr_xir_value_drop(&output);++retry_ok;
        }
        CHECK(job.context.resources==same && stats(&job.context).work>=failed.work && stats(&job.context).allocated_bytes>=failed.allocated_bytes);
        recursive_job_free(&job);
        if (!(fault%32) || fault+1==sites) fprintf(stderr,"Recursive evaluator %s owning=%u FI=%zu/%zu same-owner retry/live\n",function,owning,fault+1,sites);
    }
    fprintf(stderr,"Recursive evaluator %s owning=%u %s actual=%zu retry_ok=%zu predicted_budget_retry=%zu original128M owner unchanged\n",
        function,owning,ends?"FIRST_LAST_ONLY":"COMPLETE_FI",sites,retry_ok,retry_budget);
}
