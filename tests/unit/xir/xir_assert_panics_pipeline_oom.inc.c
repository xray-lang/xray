/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_pipeline_oom.inc.c - Typed binder pipeline and resource failure refunds
 */
static void panics_pipeline_oom(const char *directory,const char *path) {
    XrXirSourceResult result=panics_source(directory,path,
        "export fn run(){assertPanics(fn()->string{return \"owned result\"},\"normal return\")}\n",true);
    size_t live=runtime_live,bytes=runtime_bytes;XrXirArtifact *closed=NULL;
    CHECK(xr_xir_specialize(result.checked,NULL,&closed,NULL)==XR_XIR_OK);
    size_t kept=runtime_live,kept_bytes=runtime_bytes;const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    for (uint32_t group=0;group<2;++group) {
        size_t sites=0;
        for (size_t point=0;point<=sites;++point) {
            runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;XrXirArtifact *output=NULL;
            XrXirStatus status=group ? xr_xir_lower(closed,&target,NULL,&output,NULL) :
                xr_xir_specialize(result.checked,NULL,&output,NULL);
            if (!point) {CHECK(status==XR_XIR_OK);sites=runtime_attempts;}
            else CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_OUT_OF_MEMORY && !output);
            runtime_fail_at=SIZE_MAX;xr_xir_artifact_free(output);CHECK(runtime_live==kept && runtime_bytes==kept_bytes);
        }
        printf("Panics %s OOM=%zu actual full scan and refund\n",group ? "lower" : "specialize",sites);
    }
    size_t sites=0;
    for (size_t point=0;point<=sites;++point) {
        XrXirArtifact *lowered=NULL;CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
        XrXirArtifact *original=lowered;runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirProgram *program=NULL;XrXirStatus status=xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){16777216,64000000},&program);
        if (!point) {CHECK(status==XR_XIR_OK && !lowered);sites=runtime_attempts;}
        else CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_OUT_OF_MEMORY && !program && lowered==original);
        runtime_fail_at=SIZE_MAX;xr_xir_artifact_free(lowered);xr_xir_program_drop(program);
        CHECK(runtime_live==kept && runtime_bytes==kept_bytes);
    }
    printf("Panics owned VM seal OOM=%zu failed lease unchanged\n",sites);
    xr_xir_artifact_free(closed);CHECK(runtime_live==live && runtime_bytes==bytes);
    XrXirArtifact *lowered=panics_lower(&result);uint32_t entry=panics_find(xr_xir_artifact_module(lowered),"run");
    XrXirProgram *program=NULL;CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){16777216,64000000},&program)==XR_XIR_OK);
    kept=runtime_live;kept_bytes=runtime_bytes;sites=0;
    for (size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirInstance *instance=NULL;XrXirInstanceConfig config=xr_xir_instance_defaults();
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll(instance).outcome.status;
        if (!point) {CHECK(status==XR_XIR_CALL_ASSERTION);sites=runtime_attempts;}
        else {
            if (status!=XR_XIR_CALL_OOM) fprintf(stderr,"panics runtime OOM point=%zu/%zu status=%u\n",point-1,sites,status);
            CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_CALL_OOM);
        }
        runtime_fail_at=SIZE_MAX;if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(runtime_live==kept && runtime_bytes==kept_bytes);
    }
    printf("Panics runtime OOM=%zu no resource converted to success/panic; every physical baseline restored\n",sites);
    xr_xir_program_drop(program);CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
}
