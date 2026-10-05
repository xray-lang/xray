/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_inference_promises.h - One real conversion after pure type evidence
 *
 * KEY CONCEPT:
 *   Both known-context and unknown-context inference preserve the original value
 *   and emit exactly one authorized top-level callable weakening.
 */
#ifndef XIR_SOURCE_INFERENCE_PROMISES_H
#define XIR_SOURCE_INFERENCE_PROMISES_H
static void source_inference_promises(const XrXirSourceRequest *request) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest local = *request; local.entry_path = path;
    const char *programs[] = {
        ("interface Apply{apply<U>(f:fn(U)->U,value:U)->U}\n"
        "struct S implements Apply{apply<V>(f:fn(V)->V,value:V)->V{return f(value)}}\n"
        "fn forward<T:Apply>(runner:T,f:fn(i64)->i64)->i64{return runner.apply(f,41)}\n"
        "fn pure(value:i64)->i64{return value}\n"
        "export fn answer()->i64{return forward<S>(S{},pure)}\n"),
        ("interface Apply{apply<U>(value:U,f:fn(U)->U)->U}\n"
        "struct S implements Apply{apply<V>(value:V,f:fn(V)->V)->V{return f(value)}}\n"
        "fn forward<T:Apply>(runner:T,f:fn(i64)->i64)->i64{return runner.apply(41,f)}\n"
        "fn pure(value:i64)->i64{return value}\n"
        "export fn answer()->i64{return forward<S>(S{},pure)}\n")
    };
    SourceTestDeclaration records[] = {{"forward",NULL,"f",false},{"pure",NULL,NULL,true}};
    for (uint32_t mode=0;mode<2;++mode) {
        witness_promise_file(&local,"witness_promises.xr",programs[mode]);
        source_manifest_write(&local,"witness_promises.xr",records,2);
        XrXirArtifact *checked = witness_promise_check(&local,true,"inferred callable root conversion",NULL);
        const XrXirModule *module = xr_xir_compile_artifact_module(checked); uint32_t found=0;
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            if (function->name_length!=7 || memcmp(function->name,"forward",7)) continue;
            ++found; uint32_t weakens=0;
            for (uint32_t i=0;i<function->instruction_count;++i)
                weakens += function->instructions[i].op==XR_XIR_FUNCTION_WEAKEN;
            CHECK(weakens==1);
        }
        CHECK(found==1);
        witness_promise_execute(checked,mode ? "inference_known_callable" : "inference_evidence_callable",NULL);
    }
    source_manifest_raw(&local,""); CHECK(xr_test_unlink(path)==0);
}
#endif // XIR_SOURCE_INFERENCE_PROMISES_H
