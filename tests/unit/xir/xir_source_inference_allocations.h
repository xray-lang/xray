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
 *   graph, parse, proof and query owners use this same injection hook.
 */
#ifndef XIR_SOURCE_INFERENCE_ALLOCATIONS_H
#define XIR_SOURCE_INFERENCE_ALLOCATIONS_H

static void inference_source_allocation_fixture(unsigned case_id,const char *label, const char *source) {
    char directory[]="xir-inference-oom-XXXXXX",absolute[4096],path[8192];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    int length=snprintf(path,sizeof(path),"%s/root.xr",absolute);
    CHECK(length>0 && (size_t)length<sizeof(path));
    FILE *file=fopen(path,"wb"); CHECK(file);
    CHECK(fputs(source,file)>=0 && fclose(file)==0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrXirSourceRequest request={NULL,path,&authority,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
    size_t baseline=source_fixture_compile_live,sites=allocation_case_begin(case_id);
    CHECK(resolver_fault==0);
    for(size_t attempt=allocation_first();allocation_more(attempt,sites);attempt=allocation_next(attempt)) {
        CHECK(source_fixture_compile_live==baseline);
        source_fixture_compile_attempts=0; source_fixture_compile_fail_at=attempt ? attempt-1 : SIZE_MAX; source_fixture_compile_injected=false;
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=allocation_source_check(&request,&result,&diagnostic);
        if (!attempt) {
            if (status!=XR_XIR_OK) fprintf(stderr,"%s baseline: %u %s\n",label,status,diagnostic.message);
            CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
            CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
            sites=source_fixture_compile_attempts; CHECK(sites>0);
        } else {
            if (status!=XR_XIR_OUT_OF_MEMORY || result.checked || result.snapshot)
                fprintf(stderr,"%s allocation %zu/%zu: %u %s\n",label,attempt-1,sites,status,diagnostic.message);
            CHECK(source_fixture_compile_attempts>source_fixture_compile_fail_at);
            CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY);
            CHECK(!result.checked && !result.snapshot);
        }
        xr_xir_compile_source_result_free(&result);
        CHECK(source_fixture_compile_live==baseline);
            if(attempt)allocation_point(attempt-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at=SIZE_MAX; source_fixture_compile_injected=false; source_fixture_compile_attempts=0;
    CHECK(xr_test_unlink(path)==0 && xr_test_rmdir(directory)==0);
    printf("%s: %zu source/query/inference OOM sites; no partial result\n",label,sites);
}
static void inference_source_allocations(void) {
    inference_source_allocation_fixture(26,"Unit logical locals and captures",
        "fn nop(){}\nfn work(){var u:();const c=nop();const text=\"retained\";"
        "const f=fn(){u=nop();Coro.yield();const t=text;return c};defer{u=nop()};f();return u}\n"
        "export fn answer()->i64{work();return 41}\n");
    inference_source_allocation_fixture(27,"Explicit non-call Unit contexts",
        "var count=0\nfn nop(){count+=1}\n"
        "fn grouped(){return (nop())}\n"
        "fn conditional(){return true ? grouped() : nop()}\n"
        "fn matched(){return match(true){true->{conditional()},false->nop()}}\n"
        "fn invoke(f:fn()){return f()}\n"
        "export fn answer()->i64{matched();invoke(fn(){return matched()});return count+39}\n");
    inference_source_allocation_fixture(28,"Dead catch initialization region","enum E { Bad {text:string} }\nfn accept(text:string)->string{return text}\nstruct C { const value:string\n constructor(){try{}catch(E.Bad{text}){this.value=accept(text)};this.value=\"normal\"}}\nconst c=C()\n");
    inference_source_allocation_fixture(29,"Interface inference producer",
        "interface I<A>{map<U:Sendable>(seed:A,value:U)->U}\n"
        "fn forward<A,T:I<A>>(r:T,seed:A)->i64{return r.map(seed,41)}\n");
    inference_source_allocation_fixture(30,"Ordinary inference producer",
        "fn identity<T>(value:T)->T{return value}\n"
        "struct Box<A>{map<U>(value:U,body:fn(U)->U=fn(v:U)->U{return v})->U{return body(value)}\n"
        "static pick<U>(value:U)->U{return value}}\n"
        "export fn answer()->i64{return Box<string>{}.map(Box<i64>.pick(identity(41)))}\n");
}
#endif // XIR_SOURCE_INFERENCE_ALLOCATIONS_H
