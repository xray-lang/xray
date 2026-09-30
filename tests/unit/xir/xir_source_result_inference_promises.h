/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_result_inference_promises.h - Result context callable authority
 *
 * KEY CONCEPT:
 *   An authentic expected result completes unresolved own arguments without
 *   replacing supplied value evidence or the final result conversion.
 */
#ifndef XIR_SOURCE_RESULT_INFERENCE_PROMISES_H
#define XIR_SOURCE_RESULT_INFERENCE_PROMISES_H
/* Include after manifest fixture and witness promise helpers in effects-source. */
static void source_result_inference_promises(const XrXirSourceRequest *request) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest local=*request; local.entry_path=path;
    SourceTestDeclaration records[]={{"forward",NULL,"f",false},{"pure",NULL,NULL,true}};
    witness_promise_file(&local,"witness_promises.xr",
        "fn identity<T>(value:T)->T{return value}\n"
        "fn forward(f:fn()->i64)->i64{const result:fn()->i64=identity(f);return result()}\n"
        "fn pure()->i64{return 41}\n"
        "export fn answer()->i64{return forward(pure)}\n");
    source_manifest_write(&local,"witness_promises.xr",records,2);
    XrXirArtifact *checked=witness_promise_check(&local,true,"known result legal weakening",NULL);
    const XrXirModule *module=xr_xir_artifact_module(checked); uint32_t found=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length!=7 || memcmp(function->name,"forward",7)) continue;
        ++found; uint32_t weakens=0;
        for (uint32_t i=0;i<function->instruction_count;++i)
            weakens+=function->instructions[i].op==XR_XIR_FUNCTION_WEAKEN;
        CHECK(weakens==1);
    }
    CHECK(found==1); witness_promise_execute(checked,"result_inference_weakening",NULL);
    SourceTestDeclaration consume[]={{"consume",NULL,"f",false}};
    witness_promise_file(&local,"witness_promises.xr",
        "fn maker<T>()->fn(T)->T{return fn(v:T)->T{return v}}\n"
        "fn consume(f:fn(i64)->i64)->i64{return f(41)}\n"
        "export fn answer()->i64{return consume(maker())}\n");
    source_manifest_write(&local,"witness_promises.xr",consume,1);
    CHECK(!witness_promise_check(&local,false,"ordinary result cannot gain promise","result context cannot strengthen a callable promise"));
    witness_promise_file(&local,"witness_promises.xr",
        "fn wrap<T>(value:T)->Array<T>{return [value]}\n"
        "fn forward(f:fn()->i64)->i64{const a:Array<fn()->i64>=wrap(f);return 41}\n"
        "fn pure()->i64{return 41}\n"
        "export fn answer()->i64{return forward(pure)}\n");
    source_manifest_write(&local,"witness_promises.xr",records,2);
    CHECK(!witness_promise_check(&local,false,"nested result promises stay exact","expression cannot satisfy its declared type"));
    source_manifest_raw(&local,""); CHECK(xr_test_unlink(path)==0);
}
#endif // XIR_SOURCE_RESULT_INFERENCE_PROMISES_H
