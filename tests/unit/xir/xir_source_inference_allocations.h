/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_inference_allocations.h - Isolated inference producer failure sweeps
 *
 * KEY CONCEPT:
 *   Every counted source/query/inference allocation failure publishes no result;
 *   parsed graph and linked proof allocations are outside this injection hook.
 */
#ifndef XIR_SOURCE_INFERENCE_ALLOCATIONS_H
#define XIR_SOURCE_INFERENCE_ALLOCATIONS_H

static void inference_source_allocation_fixture(XrCompilerSession *session,
    const char *label, const char *source) {
    char directory[]="xir-inference-oom-XXXXXX",absolute[4096],path[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    int length=snprintf(path,sizeof(path),"%s/root.xr",absolute);
    CHECK(length>0 && (size_t)length<sizeof(path));
    FILE *file=fopen(path,"wb"); CHECK(file);
    CHECK(fputs(source,file)>=0 && fclose(file)==0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrXirSourceRequest request={session,path,&authority,NULL,NULL,NULL};
    size_t baseline=live,sites=0;
    CHECK(resolver_fault==0);
    for (size_t attempt=0;attempt<=sites;++attempt) {
        CHECK(live==baseline);
        attempts=0; fail_at=attempt ? attempt-1 : SIZE_MAX;
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
        if (!attempt) {
            if (status!=XR_XIR_OK) fprintf(stderr,"%s baseline: %u %s\n",label,status,diagnostic.message);
            CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
            CHECK(xr_xir_source_snapshot_view(result.snapshot)->complete);
            sites=attempts; CHECK(sites>0);
        } else {
            if (status!=XR_XIR_OUT_OF_MEMORY || result.checked || result.snapshot)
                fprintf(stderr,"%s allocation %zu/%zu: %u %s\n",label,attempt-1,sites,status,diagnostic.message);
            CHECK(attempts>fail_at);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY);
            CHECK(!result.checked && !result.snapshot);
        }
        xr_xir_source_result_free(&result);
        CHECK(live==baseline);
    }
    fail_at=SIZE_MAX; attempts=0;
    CHECK(xr_test_unlink(path)==0 && xr_test_rmdir(directory)==0);
    printf("%s: %zu source/query/inference OOM sites; no partial result\n",label,sites);
}
static void inference_source_allocations(XrCompilerSession *session) {
    inference_source_allocation_fixture(session,"Interface inference producer",
        "interface I<A>{map<U:Sendable>(seed:A,value:U)->U}\n"
        "fn forward<A,T:I<A>>(r:T,seed:A)->i64{return r.map(seed,41)}\n");
    inference_source_allocation_fixture(session,"Ordinary inference producer",
        "fn identity<T>(value:T)->T{return value}\n"
        "struct Box<A>{map<U>(value:U,body:fn(U)->U=fn(v:U)->U{return v})->U{return body(value)}\n"
        "static pick<U>(value:U)->U{return value}}\n"
        "export fn answer()->i64{return Box<string>{}.map(Box<i64>.pick(identity(41)))}\n");
}
#endif // XIR_SOURCE_INFERENCE_ALLOCATIONS_H
