/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_promises.h - Authority of bound method effects
 *
 * KEY CONCEPT:
 *   Callback qualification and callee effects remain independent contracts.
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_PROMISES_H
#define XIR_SOURCE_REQUIREMENT_VALUE_PROMISES_H
static void source_requirement_value_promises(const XrXirSourceRequest *request, const char *output) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest local = *request; local.entry_path = path;
    const char *direct =
        "interface Map { map<U:Sendable>(value:U)->U }\n"
        "struct Runner implements Map { map<V:Sendable>(value:V)->V{return value} }\n"
        "fn invoke(f:fn(i64)->i64)->i64{return f(41)}\n"
        "fn forward<T:Map>(runner:T)->i64{return invoke(runner.map<i64>)}\n"
        "export fn answer()->i64{return forward<Runner>(Runner{})}\n";
    witness_promise_file(&local,"witness_promises.xr",direct);
    SourceTestDeclaration records[] = {{"map","Map",NULL,true},
        {"invoke",NULL,"f",true},{"forward",NULL,NULL,true},{"map","Runner",NULL,true}};
    source_manifest_write(&local,"witness_promises.xr",records,3);
    witness_promise_execute(witness_promise_check(&local,true,"bound original method promise",NULL),
        "witness_value_direct",output);
    /* A stronger implementation cannot qualify the abstract function value. */
    source_manifest_write(&local,"witness_promises.xr",records+1,3);
    CHECK(!witness_promise_check(&local,false,"concrete promise cannot strengthen bound requirement",
        "qualified reference requires an explicit requirement promise"));
    const char *callback =
        "interface Apply { apply<U:Sendable>(value:U,f:fn(U)->U)->U }\n"
        "struct Runner implements Apply { apply<V:Sendable>(value:V,f:fn(V)->V)->V{return f(value)} }\n"
        "fn forward<T:Apply>(runner:T,f:fn(i64)->i64)->i64{const bound=runner.apply<i64>;return bound(41,f)}\n"
        "fn pure(value:i64)->i64{return value}\n"
        "export fn answer()->i64{return forward<Runner>(Runner{},pure)}\n";
    witness_promise_file(&local,"witness_promises.xr",callback);
    SourceTestDeclaration nested[] = {{"apply","Apply","f",true},
        {"forward",NULL,"f",false},{"pure",NULL,NULL,true}};
    source_manifest_write(&local,"witness_promises.xr",nested,3);
    witness_promise_execute(witness_promise_check(&local,true,"bound nested callback promise",NULL),
        "witness_value_callback",output);
    SourceTestDeclaration missing_callback[] = {nested[0],nested[2]};
    source_manifest_write(&local,"witness_promises.xr",missing_callback,2);
    CHECK(!witness_promise_check(&local,false,"bound callback lacks original nested promise",
        "callable conversion may only weaken its outer execution contract"));
    nested[1].parameter = "f"; nested[1].no_suspend = true;
    source_manifest_write(&local,"witness_promises.xr",nested,3);
    CHECK(!witness_promise_check(&local,false,"callback promise does not qualify ordinary bound callee",
        "declared no_suspend"));
    source_manifest_raw(&local,""); CHECK(xr_test_unlink(path)==0);
}
#endif // XIR_SOURCE_REQUIREMENT_VALUE_PROMISES_H
