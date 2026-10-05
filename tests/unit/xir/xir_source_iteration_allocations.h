/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_iteration_allocations.h - Array snapshot iteration producer failure sites
 *
 * KEY CONCEPT:
 *   Fail every owned source allocation without publishing a partial artifact.
 */
#ifndef XIR_SOURCE_ITERATION_ALLOCATIONS_H
#define XIR_SOURCE_ITERATION_ALLOCATIONS_H
static void source_iteration_allocations(void){
    char directory[]="iteration-source-XXXXXX",absolute[4096],path[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(path,sizeof(path),"%s/root.xr",absolute)>0);
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fputs("export fn answer()->i64{var xs=[10,11,20];var n=0;for(i,x in xs){defer{n+=0};xs.push(99);if(i==1){n+=x;continue};n+=x};return n}\n",file)>=0 && fclose(file)==0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrXirSourceRequest request={NULL,path,&authority,NULL,XR_SOURCE_STDLIB,NULL, XR_XIR_PROGRAM, NULL};size_t sites=allocation_case_begin(25);
    for(size_t site=allocation_first();allocation_more(site,sites);site=allocation_next(site)){
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=site?site-1:SIZE_MAX; source_fixture_compile_injected=false;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=allocation_source_check(&request,&result,&diagnostic);
        if(!site){CHECK(status==XR_XIR_OK && result.checked && result.snapshot);sites=source_fixture_compile_attempts;}
        else{CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY);CHECK(!result.checked && !result.snapshot);}
        xr_xir_compile_source_result_free(&result);CHECK(!source_fixture_compile_live);
            if(site)allocation_point(site-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at=SIZE_MAX; source_fixture_compile_injected=false;CHECK(xr_test_unlink(path)==0 && xr_test_rmdir(directory)==0);
    printf("Array iteration source producer: %zu OOM sites, zero owned allocations\n",sites);
}

#endif
