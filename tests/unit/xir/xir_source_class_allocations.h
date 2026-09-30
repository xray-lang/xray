/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_class_allocations.h - Class identity producer failure sites
 *
 * KEY CONCEPT:
 *   Fail every owned source allocation without publishing a partial artifact.
 */
#ifndef XIR_SOURCE_CLASS_ALLOCATIONS_H
#define XIR_SOURCE_CLASS_ALLOCATIONS_H
static void source_class_allocations(XrCompilerSession *session){
    char directory[]="class-source-XXXXXX",absolute[4096],path[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(path,sizeof(path),"%s/root.xr",absolute)>0);
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fputs("final class Counter<T>{private n:i64;const label:string;constructor(n:i64,b:bool){this.n=n;if(b){this.label=\"counter\"}else{this.label=\"other\"}};add()->i64{this.n+=1;return this.n};static make()->Counter<T>{return Counter<T>(40,true)}}\nexport fn answer()->i64{const first=Counter<string>.make();const alias=first;return alias.add()}\n",file)>=0 && fclose(file)==0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrXirSourceRequest request={session,path,&authority,NULL,XR_SOURCE_STDLIB,NULL};size_t sites=0;
    for(size_t site=0;site<=sites;++site){
        attempts=0;fail_at=site?site-1:SIZE_MAX;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
        if(!site){CHECK(status==XR_XIR_OK && result.checked && result.snapshot);sites=attempts;}
        else{CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY);CHECK(!result.checked && !result.snapshot);}
        xr_xir_source_result_free(&result);CHECK(!live);
    }
    fail_at=SIZE_MAX;CHECK(xr_test_unlink(path)==0 && xr_test_rmdir(directory)==0);
    printf("class identity source producer: %zu OOM sites, zero owned allocations\n",sites);
}

#endif
