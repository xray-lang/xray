/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_contextual_lambda_cases.h - Authentic complete lambda contexts
 *
 * KEY CONCEPT:
 *   Prior evidence supplies a full callable signature; closure body checking
 *   retains the caller binder and proves the original callable contract.
 */
#ifndef XIR_SOURCE_CONTEXTUAL_LAMBDA_PROMISES_H
#define XIR_SOURCE_CONTEXTUAL_LAMBDA_PROMISES_H
static void source_contextual_lambda_promises(const XrXirSourceRequest *request) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest local=*request; local.entry_path=path;
    SourceTestDeclaration records[]={{"run",NULL,"f",false}};
    witness_promise_file(&local,"witness_promises.xr",
        "fn run(f:fn(i64)->i64)->i64{return f(41)}\n"
        "export fn answer()->i64{return run(fn(v){return v})}\n");
    source_manifest_write(&local,"witness_promises.xr",records,1);
    witness_promise_execute(witness_promise_check(&local,true,"contextual lambda proves promise",NULL),
        "contextual_lambda_promise",NULL);
    witness_promise_file(&local,"witness_promises.xr",
        "fn unknown(v:i64)->i64{return v}\n"
        "fn run(f:fn(i64)->i64)->i64{return f(41)}\n"
        "fn invoke(g:fn(i64)->i64)->i64{return run(fn(v){return g(v)})}\n"
        "export fn answer()->i64{return invoke(unknown)}\n");
    CHECK(!witness_promise_check(&local,false,"context cannot grant a body promise","declared no_suspend"));
    source_manifest_raw(&local,""); CHECK(xr_test_unlink(path)==0);
}
#endif // XIR_SOURCE_CONTEXTUAL_LAMBDA_PROMISES_H
