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
    assert_compile_stage_cases(result.checked,ASSERT_COMPILE_SPECIALIZE,"Assertion specialize/replay");
    assert_compile_stage_cases(result.checked,ASSERT_COMPILE_LOWER,"Assertion lower/replay");
    assert_compile_stage_cases(result.checked,ASSERT_COMPILE_TAKE,"Assertion owned VM take/replay");
    size_t kept=0,kept_bytes=0,sites=0;
    XrXirArtifact *lowered=panics_lower(&result);uint32_t entry=panics_find(xr_xir_compile_artifact_module(lowered),"run");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
    kept=runtime_live;kept_bytes=runtime_bytes;sites=0;
    for (size_t point=0;point<=sites;++point) {
        runtime_attempts=0;runtime_fail_at=point ? point-1 : SIZE_MAX;
        XrXirInstance *instance=NULL;XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
        if (!point) {CHECK(status==XR_XIR_CALL_ASSERTION);sites=runtime_attempts;}
        else {
            if (status!=XR_XIR_CALL_OOM) fprintf(stderr,"panics runtime OOM point=%zu/%zu status=%u\n",point-1,sites,status);
            CHECK(runtime_attempts>runtime_fail_at && status==XR_XIR_CALL_OOM);
        }
        runtime_fail_at=SIZE_MAX;if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        CHECK(runtime_live==kept && runtime_bytes==kept_bytes);
    }
    printf("Panics runtime OOM=%zu no resource converted to success/panic; every physical baseline restored\n",sites);
    xr_xir_compile_program_drop(program);CHECK(!assert_compile_extra_blocks() && !assert_compile_extra_bytes() && !runtime_live && !runtime_bytes);
}
