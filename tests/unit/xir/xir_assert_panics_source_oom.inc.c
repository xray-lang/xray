/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_source_oom.inc.c - Source, binder and default failure publication
 */
static XrXirStatus panics_source_attempt(const XrXirSourceRequest *request,XrXirSourceResult *result) {
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_source_check(request,result,&diagnostic);
    if (status!=XR_XIR_OK && status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
        fprintf(stderr,"panics Source failed %u %s\n",status,diagnostic.message);
    return status;
}
static void panics_source_oom(const char *directory,const char *path) {
    panics_write(path,"export fn run(){assertPanics(fn(){var zero=0;var ignored=1/zero})}\n");
    XrCompilerSession *session=xr_compiler_session_new(NULL);CHECK(session);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceRequest request={session,path,&authority,NULL,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};source_attempts=runtime_attempts=0;source_peak=source_bytes;
    CHECK(panics_source_attempt(&request,&result)==XR_XIR_OK);
    size_t sites=source_attempts,core_sites=runtime_attempts;
    size_t baseline=source_live,bytes=source_bytes,core_baseline=runtime_live,core_bytes=runtime_bytes;
    for (uint32_t group=0;group<2;++group) {
        for (size_t fault=0;fault<(group ? core_sites : sites);++fault) {
            XrCompilerSession *failed_session=xr_compiler_session_new(NULL);CHECK(failed_session);
            XrXirSourceRequest failed_request=request;failed_request.session=failed_session;
            XrXirSourceResult failed={0};source_attempts=runtime_attempts=0;
            if (group) runtime_fail_at=fault; else source_fail_at=fault;
            XrXirStatus status=panics_source_attempt(&failed_request,&failed);
            source_fail_at=runtime_fail_at=SIZE_MAX;
            if (status!=XR_XIR_OUT_OF_MEMORY) fprintf(stderr,"panics Source OOM group=%u point=%zu/%zu status=%u\n",
                group,fault,group ? core_sites : sites,status);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !failed.checked && !failed.snapshot);
            xr_xir_source_result_free(&failed);xr_compiler_session_delete(failed_session);
            CHECK(source_live==baseline && source_bytes==bytes && runtime_live==core_baseline && runtime_bytes==core_bytes);
        }
        printf("Panics Source OOM group=%u actual sites=%zu complete baseline refund\n",group,group ? core_sites : sites);fflush(stdout);
    }
    xr_xir_source_result_free(&result);CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    XrXirBudget defaults=xr_xir_default_budget();uint64_t low=0,high=defaults.metadata_bytes;
    while (low<high) {
        uint64_t middle=low+(high-low)/2;XrXirBudget budget=defaults;budget.metadata_bytes=middle;
        request.budget=&budget;source_peak=source_bytes;
        XrXirSourceResult bounded={0};XrXirStatus status=panics_source_attempt(&request,&bounded);
        CHECK(source_peak<=budget.metadata_bytes && (status==XR_XIR_OK || status==XR_XIR_BUDGET));
        xr_xir_source_result_free(&bounded);CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
        if (status==XR_XIR_OK) high=middle; else low=middle+1;
    }
    CHECK(low>0);
    for (uint32_t minus=0;minus<2;++minus) {
        XrXirBudget budget=defaults;budget.metadata_bytes=low-minus;request.budget=&budget;source_peak=source_bytes;
        XrXirSourceResult bounded={0};XrXirStatus status=panics_source_attempt(&request,&bounded);
        CHECK(status==(minus ? XR_XIR_BUDGET : XR_XIR_OK) && source_peak<=budget.metadata_bytes);
        if (minus) CHECK(!bounded.checked && !bounded.snapshot);
        xr_xir_source_result_free(&bounded);CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    }
    printf("Panics whole Source metadata exact=%llu/minus1 Source-scope peaks bounded\n",(unsigned long long)low);
    xr_compiler_session_delete(session);
}
