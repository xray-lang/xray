/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_result_inference_cases.h - Result evidence after supplied operands
 *
 * KEY CONCEPT:
 *   An authentic expected result completes unresolved own arguments without
 *   replacing supplied value evidence or the final result conversion.
 */
#ifndef XIR_SOURCE_RESULT_INFERENCE_CASES_H
#define XIR_SOURCE_RESULT_INFERENCE_CASES_H
/* Include after xir_source_generic_requirement_cases.h in source-query tests. */
static void source_result_inference_cases(XrXirSourceRequest *request) {
    const char *programs[]={
        /* Annotation. */
        ("fn empty<T>()->Array<T>{return []}\n"
        "export fn genericMethodNumber()->i64{const a:Array<i64>=empty();return 41+len(a)}\n"
        "export fn genericMethodText()->string{const a:Array<string>=empty();return \"mapped\"}\n"),
        /* Assignment cannot change its established element type. */
        ("fn empty<T>()->Array<T>{return []}\n"
        "export fn genericMethodNumber()->i64{var a:Array<i64>=[0];a=empty();return 41+len(a)}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n"),
        /* Explicit return including symbolic caller evidence. */
        ("fn empty<T>()->Array<T>{return []}\n"
        "fn forwarded<A>()->Array<A>{return (empty())}\n"
        "export fn genericMethodNumber()->i64{return 41+len(forwarded<i64>())}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n"),
        /* Nested result and independently solved T/unknown U. */
        ("fn nested<T>()->Array<Array<T>>{return []}\n"
        "fn make<T,U>(seed:T)->Array<U>{return []}\n"
        "export fn genericMethodNumber()->i64{const a:Array<Array<i64>>=nested();const b:Array<string>=make(41);return 41+len(a)+len(b)}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n"),
        /* Fixed nominal prefix, READ and static consumers. */
        ("struct S<X>{empty<U>()->Array<U>{return []} static make<V>()->Array<V>{return []}}\n"
        "export fn genericMethodNumber()->i64{const a:Array<i64>=S<string>{}.empty();const b:Array<string>=S<i64>.make();return 41+len(a)+len(b)}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n"),
        /* Original interface owner with nonempty parent and method binders. */
        ("interface Empty<A>{empty<U>()->Array<U>}\n"
        "struct S<X> implements Empty<X>{empty<V>()->Array<V>{return []}}\n"
        "fn apply<A,T:Empty<A>>(value:T)->Array<A>{return value.empty()}\n"
        "export fn genericMethodNumber()->i64{return 41+len(apply<i64,S<i64>>(S<i64>{}))}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n"),
        /* Known result must retain widening, rather than a new exact equation. */
        ("fn identity<T>(value:T)->T{return value}\n"
        "export fn genericMethodNumber()->i64{const n:i8=41;return identity(n)}\n"
        "export fn genericMethodText()->string{return identity(\"mapped\")}\n"),
        /* Supplied expression runs once in original runtime order before the default body. */
        ("var trace=0\n"
        "fn first()->i64{trace=trace*10+1;return 0}\n"
        "fn last()->i64{trace=trace*10+2;return 0}\n"
        "fn make<T,U>(seed:T,extra:i64=last())->Array<U>{return []}\n"
        "export fn genericMethodNumber()->i64{const a:Array<string>=make(first());if(trace==12){return 41+len(a)}else{return 0}}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n")
    };
    for (uint32_t i=0;i<sizeof(programs)/sizeof(*programs);++i)
        source_generic_requirement_run(request,programs[i]);
    const char *rejected[]={
        "fn empty<T>()->Array<T>{return []}\nfn bad(){const a=empty()}\n",
        "fn empty<T,U>()->Array<T>{return []}\nfn bad()->Array<i64>{return empty()}\n",
        "fn wrap<T>(value:T)->Array<T>{return [value]}\nfn bad()->Array<string>{return wrap(41)}\n",
        "struct Pair<A,B>{first:A;rest:Array<B>}\nfn make<T,U>(v:T)->Pair<T,U>{return Pair<T,U>{first:v,rest:[]}}\nfn bad()->Pair<string,i64>{return make(41)}\n",
        "interface Evidence{}\nfn empty<T:Evidence>()->Array<T>{return []}\nfn bad()->Array<i64>{return empty()}\n",
        "fn empty<T:Sendable>()->Array<T>{return []}\nfn bad<A>()->Array<A>{return empty()}\n",
        "fn consume<T>(f:fn(T)->T)->i64{return 41}\nfn bad()->i64{return consume(fn(v){return v})}\n",
        "fn identity<T>(value:T)->T{return value}\nfn bad()->i8{const value:i64=41;return identity(value)}\n",
        "struct S<X>{make<U>(seed:X)->Array<U>{return []}}\nfn bad()->Array<i64>{return S<string>{}.make(0)}\n"
    };
    const char *reasons[]={
        "cannot infer all declaration type arguments; supply an explicit list",
        "cannot infer all declaration type arguments; supply an explicit list",
        "expression cannot satisfy its declared type",
        "expression cannot satisfy its declared type",
        "type argument does not prove the declared constraint",
        "type argument does not prove the declared constraint",
        "closure parameter requires an annotation or complete callable context",
        "expression cannot satisfy its declared type",
        "expression cannot satisfy its declared type",
    };
    _Static_assert(sizeof(rejected)/sizeof(*rejected)==sizeof(reasons)/sizeof(*reasons), "Each rejection has its diagnostic");
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        write_source(request->entry_path,rejected[i]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if (status!=XR_XIR_BAD_TYPE || !strstr(diagnostic.message,reasons[i])) fprintf(stderr,"result inference rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot && strstr(diagnostic.message,reasons[i]));
        CHECK(!result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
}
#endif // XIR_SOURCE_RESULT_INFERENCE_CASES_H
