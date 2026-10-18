/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_runtime.h - Real allocation failures and cancellation release all owners
 */
typedef struct LibraryRuntimeMetrics {
    uint64_t value_peak,requested_value,work,requested_call,metadata_peak,call_peak;
} LibraryRuntimeMetrics;
#include "library_runtime_retry.h"
static XrXirCallStatus library_native_runtime_job_limits(LibraryNativeOwner *owner,XrXirProgram *program,
    size_t cancel_at,size_t *polls,const XrXirInstanceConfig *limits,LibraryRuntimeMetrics *metrics) {
    CHECK(!owner->instances[0] && !owner->instances[1] && !owner->releases);
    memset(owner->steps,0,sizeof(owner->steps));memset(owner->crossings,0,sizeof(owner->crossings));*polls=0;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    unsigned active=0;
    for(unsigned i=0;i<2;++i) {
        active=i;
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        if(limits) config=*limits;
        status=xr_xir_instance_new(program,&config,&owner->instances[i]);
        if(status!=XR_XIR_CALL_READY) {CHECK((status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT) && !owner->instances[i]);goto finish;}
    }
    for(unsigned i=0;i<2;++i) {
        active=i;
        status=xr_xir_instance_start(owner->instances[i],owner->run,NULL,0);
        if(status!=XR_XIR_CALL_READY) {CHECK(status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);goto finish;}
        do {
            if(*polls==cancel_at) {
                XrXirCallStatus requested=xr_xir_instance_cancel_current(owner->instances[i]);
                CHECK(requested==XR_XIR_CALL_READY || requested==XR_XIR_CALL_CANCEL_REQUESTED);
                status=xr_xir_instance_poll_bounded(owner->instances[i],UINT64_MAX).outcome.status;
                CHECK(status==XR_XIR_CALL_CANCELLED);goto finish;
            }
            CHECK(*polls<65536);++*polls;
            status=xr_xir_instance_poll_bounded(owner->instances[i],1).outcome.status;
        } while(status==XR_XIR_CALL_READY);
        if(status!=XR_XIR_CALL_RETURNED) {CHECK(status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);goto finish;}
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(owner->instances[i],&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);
        CHECK(owner->steps[i][1] && (!owner->mode || (owner->steps[i][0] && owner->crossings[i]>=2)));
        CHECK(owner->mode || !owner->steps[i][0]);
    }
finish:;
    if((status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_CANCELLED) && owner->instances[active])
        library_native_runtime_recover(owner,active,status);
    /* Cleanup must not allocate, even when the actual failed allocation or
     * cancellation happened during genuine module initialization. */
    size_t saved_runtime=runtime_fail_at,saved_compiler=instance_compile_fail_at;
    size_t runtime_before=runtime_attempts,compiler_before=instance_compile_attempts;
    runtime_fail_at=runtime_before;instance_compile_fail_at=compiler_before;
    for(unsigned i=0;i<2;++i) if(owner->instances[i]) {
        XrXirCallStatus stopped=xr_xir_instance_stop(owner->instances[i]);
        CHECK(stopped==XR_XIR_CALL_READY || (status==XR_XIR_CALL_OOM && stopped==XR_XIR_CALL_OOM) ||
            (status==XR_XIR_CALL_LIMIT && stopped==XR_XIR_CALL_LIMIT));
        if(metrics) {
            XrXirDomainBudgetStats budget=xr_xir_domain_budget_stats(owner->instances[i]->domain);
            XrXirDomainStats value=xr_xir_domain_stats(owner->instances[i]->domain);
            if(value.peak_bytes>metrics->value_peak) metrics->value_peak=value.peak_bytes;
            if(budget.requested_bytes>metrics->requested_value) metrics->requested_value=budget.requested_bytes;
            if(budget.work>metrics->work) metrics->work=budget.work;
            if(budget.requested_call_bytes>metrics->requested_call) metrics->requested_call=budget.requested_call_bytes;
            if(budget.metadata_peak>metrics->metadata_peak) metrics->metadata_peak=budget.metadata_peak;
            if(budget.call_peak>metrics->call_peak) metrics->call_peak=budget.call_peak;
        }
        XrXirCallStatus freed=xr_xir_instance_free(owner->instances[i]);owner->instances[i]=NULL;
        CHECK(freed==XR_XIR_CALL_READY || (status==XR_XIR_CALL_OOM && freed==XR_XIR_CALL_OOM) ||
            (status==XR_XIR_CALL_LIMIT && freed==XR_XIR_CALL_LIMIT) ||
            (status==XR_XIR_CALL_CANCELLED && freed==XR_XIR_CALL_CANCELLED));
    }
    CHECK(runtime_attempts==runtime_before && instance_compile_attempts==compiler_before);
    runtime_fail_at=saved_runtime;instance_compile_fail_at=saved_compiler;
    return status;
}
static XrXirCallStatus library_native_runtime_job(LibraryNativeOwner *owner,XrXirProgram *program,
    size_t cancel_at,size_t *polls) {
    return library_native_runtime_job_limits(owner,program,cancel_at,polls,NULL,NULL);
}
static void library_native_runtime_restored(size_t runtime_blocks,size_t runtime_size,
    size_t compiler_blocks,size_t compiler_size) {
    if(runtime_live!=runtime_blocks || runtime_bytes!=runtime_size) runtime_report_residuals(stderr);
    CHECK(runtime_live==runtime_blocks && runtime_bytes==runtime_size &&
        instance_compile_live==compiler_blocks && instance_compile_bytes==compiler_size);
}
#include "library_runtime_bounds.h"
static void library_native_runtime_resources(LibraryNativeOwner *owner,XrXirProgram *program,
    const char *mode,const char *option) {
    bool full=!strcmp(option,"--runtime-faults"),cancel=!strcmp(option,"--cancel"),bounds=!strcmp(option,"--runtime-bounds");
    CHECK(full || cancel || bounds || !strcmp(option,"--runtime-prefix"));
    size_t runtime_blocks=runtime_live,runtime_size=runtime_bytes,compiler_blocks=instance_compile_live,compiler_size=instance_compile_bytes;
    size_t start=runtime_attempts,polls=0;
    CHECK(library_native_runtime_job(owner,program,SIZE_MAX,&polls)==XR_XIR_CALL_RETURNED);
    size_t sites=runtime_attempts-start;CHECK(sites && polls);
    library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
    if(bounds) {
        library_native_runtime_bounds(owner,program,runtime_blocks,runtime_size,compiler_blocks,compiler_size);
    } else if(cancel) {
        for(size_t at=0;at<polls;++at) {
            size_t actual=0;CHECK(library_native_runtime_job(owner,program,at,&actual)==XR_XIR_CALL_CANCELLED && actual==at);
            library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
            CHECK(library_native_runtime_job(owner,program,SIZE_MAX,&actual)==XR_XIR_CALL_RETURNED && actual==polls);
            library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
        }
    } else for(size_t at=0;at<sites;++at) {
        if(!full && at && at!=sites-1) continue;
        size_t actual=0;runtime_fail_at=runtime_attempts+at;
        CHECK(library_native_runtime_job(owner,program,SIZE_MAX,&actual)==XR_XIR_CALL_OOM && runtime_attempts>runtime_fail_at);
        runtime_fail_at=SIZE_MAX;
        library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
        CHECK(library_native_runtime_job(owner,program,SIZE_MAX,&actual)==XR_XIR_CALL_RETURNED && actual==polls);
        library_native_runtime_restored(runtime_blocks,runtime_size,compiler_blocks,compiler_size);
        fprintf(stderr,"Library runtime %s FI%zu/%zu same owner/retry restored\n",mode,at+1,sites);
    }
    size_t runtime_before=runtime_attempts,compiler_before=instance_compile_attempts;
    runtime_fail_at=runtime_before;instance_compile_fail_at=compiler_before;
    xr_xir_compile_program_drop(program);
    CHECK(runtime_attempts==runtime_before && instance_compile_attempts==compiler_before);
    runtime_fail_at=SIZE_MAX;instance_compile_fail_at=SIZE_MAX;
    CHECK(owner->releases==1 && !runtime_live && !runtime_bytes && !instance_compile_live && !instance_compile_bytes);
    fprintf(stderr,"Library runtime %s %s actual_sites%zu cancellation_boundaries%zu real Instances42 final drop allocation-free physical0\n",
        mode,option,sites,polls);
}
