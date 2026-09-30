/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_generic_requirement_promises.h - Generic abstract callable promises
 *
 * KEY CONCEPT:
 *   A manifest qualifies the original generic requirement for every specialization.
 */
#ifndef XIR_SOURCE_GENERIC_REQUIREMENT_PROMISES_H
#define XIR_SOURCE_GENERIC_REQUIREMENT_PROMISES_H

/* Included after the source witness manifest and execution helpers. */
static void source_generic_requirement_promises(const XrXirSourceRequest *request, const char *output) {
    char path[8192]; source_fixture_path(request,"witness_promises.xr",path);
    XrXirSourceRequest probe = *request; probe.entry_path = path;
    request = &probe;
    const char *program =
        "interface Apply<A> { apply<U>(seed:A,value:U,f:fn(U)->U)->U }\n"
        "struct Runner<X,Y> implements Apply<X> { apply<V>(seed:X,value:V,f:fn(V)->V)->V{return f(value)} }\n"
        "fn forward<A,T:Apply<A>,U>(runner:T,seed:A,value:U,f:fn(U)->U)->U{return runner.apply<U>(seed,value,f)}\n"
        "fn pure(value:i64)->i64{return value}\n"
        "export fn answer()->i64{return forward<i64,Runner<i64,string>,i64>(Runner<i64,string>{},0,41,pure)}\n";
    witness_promise_file(request,"witness_promises.xr",program);
    SourceTestDeclaration records[] = {{"apply","Apply","f",true},
        {"forward",NULL,"f",true},{"pure",NULL,NULL,true}};
    source_manifest_write(request,"witness_promises.xr",records,3);
    witness_promise_execute(witness_promise_check(request,true,"generic original callable promise",NULL),
        "witness_generic_method",output);
    records[1].parameter = NULL;
    source_manifest_write(request,"witness_promises.xr",records,3);
    CHECK(!witness_promise_check(request,false,"generic unqualified callback",
        "callable conversion may only discard its top-level promise"));
    records[1].parameter = "f"; records[0].no_suspend = false;
    source_manifest_write(request,"witness_promises.xr",records,3);
    CHECK(!witness_promise_check(request,false,"generic unknown abstract effect","declared no_suspend"));
    source_manifest_raw(request,"");
    CHECK(xr_test_unlink(path) == 0);
}
#endif // XIR_SOURCE_GENERIC_REQUIREMENT_PROMISES_H
