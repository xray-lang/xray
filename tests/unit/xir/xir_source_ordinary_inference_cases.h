/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_ordinary_inference_cases.h - Shared free and nominal own inference
 *
 * KEY CONCEPT:
 *   Ordinary calls use the same evidence engine and execute defaults only after
 *   supplied values determine every own type argument.
 */
#ifndef XIR_SOURCE_ORDINARY_INFERENCE_CASES_H
#define XIR_SOURCE_ORDINARY_INFERENCE_CASES_H
/* Include after xir_source_generic_requirement_cases.h for its owned VM runner. */
static void source_ordinary_inference_cases(XrXirSourceRequest *request) {
    source_generic_requirement_run(request,
        "fn choose<T,U>(value:T,body:fn(T)->T,tag:U)->T{return body(value)}\n"
        "fn identity<T>(value:T)->T{return value}\n"
        "export fn genericMethodNumber()->i64{return choose(40,fn(v){return v+1},\"tag\")}\n"
        "export fn genericMethodText()->string{return identity(\"mapped\")}\n");
    source_generic_requirement_run(request,
        "struct S<X>{map<U>(seed:X,value:U,body:fn(U)->U=fn(v:U)->U{return v})->U{return body(value)} "
        "static choose<V>(seed:X,value:V,body:fn(V)->V=fn(v:V)->V{return v})->V{return body(value)}}\n"
        "export fn genericMethodNumber()->i64{return S<string>.choose(\"tag\",40,fn(v){return v+1})}\n"
        "export fn genericMethodText()->string{return S<i64>{}.map(0,\"mapped\")}\n");
    source_generic_requirement_run(request,
        "var trace=0\n"
        "fn defaultStamp()->i64{trace=trace*10+3;return 0}\n"
        "struct S<X>{map<U>(value:U,ignored:i64=defaultStamp())->U{return value}}\n"
        "fn receiver<X>(r:S<X>)->S<X>{trace=trace*10+1;return r}\n"
        "fn argument()->i64{trace=trace*10+2;return 41}\n"
        "export fn genericMethodNumber()->i64{const v=receiver(S<string>{}).map(argument());if (trace==123){return v};return 0}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    const char *rejected[]={
        "fn id<T:Sendable>(value:T)->T{return value}\nfn bad<A>(value:A)->A{return id(value)}\n",
        "fn first<T,U>(value:T,ignored:i64=0)->T{return value}\nfn bad()->i64{return first(41)}\n",
        "fn none<T>()->i64{return 41}\nfn bad()->i64{return none()}\n",
        "fn pair<T>(a:T,b:T)->T{return a}\nfn bad()->i64{return pair(41,\"wrong\")}\n",
        "struct S<X>{map<U>(seed:X,value:U)->U{return value}}\nfn bad()->i64{return S<string>{}.map(0,41)}\n",
        "struct S<X>{static choose<U,V>(seed:X,value:U)->U{return value}}\nfn bad()->i64{return S<string>.choose(\"tag\",41)}\n",
        "fn consume<T>(body:fn(T)->T)->i64{return 41}\nfn bad()->i64{return consume(fn(v){return v})}\n"
    };
    const char *expected_messages[]={
        "type argument does not prove the declared constraint", /* missing caller Sendable proof */
        "cannot infer all declaration type arguments; supply an explicit list", /* phantom U cannot come from default */
        "cannot infer all declaration type arguments; supply an explicit list", /* phantom T has no value evidence */
        "expression cannot satisfy its declared type", /* second value fails known i64 context */
        "expression cannot satisfy its declared type", /* nominal string prefix cannot change */
        "cannot infer all declaration type arguments; supply an explicit list", /* static phantom V remains unresolved */
        "closure parameter requires an annotation or complete callable context", /* lambda own type has no argument or result evidence */
    };
    _Static_assert(sizeof(expected_messages)/sizeof(*expected_messages)==sizeof(rejected)/sizeof(*rejected), "Every rejection needs an exact diagnostic");
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        write_source(request->entry_path,rejected[i]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if (status!=XR_XIR_BAD_TYPE) fprintf(stderr,"ordinary inference rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot);
        if (strcmp(diagnostic.message,expected_messages[i]))
            fprintf(stderr,"inference diagnostic %u: expected '%s', got '%s'\n",
                i,expected_messages[i],diagnostic.message);
        CHECK(strcmp(diagnostic.message,expected_messages[i])==0);
        CHECK(!result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
}
#endif // XIR_SOURCE_ORDINARY_INFERENCE_CASES_H
