/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_inference_cases.h - Single-evaluation own inference
 *
 * KEY CONCEPT:
 *   Real source evidence becomes the same owned Checked tuple as explicit calls.
 */
#ifndef XIR_SOURCE_REQUIREMENT_INFERENCE_CASES_H
#define XIR_SOURCE_REQUIREMENT_INFERENCE_CASES_H
/* After xir_source_generic_requirement_cases.h; its runner destroys source and
 * packet producer before specializing/executing independent 41 and string checks. */
static void source_requirement_inference_positive(XrXirSourceRequest *request) {
    source_generic_requirement_run(request,
        "interface Relay<A>{map<U:Sendable>(seed:A,value:U)->U}\n"
        "struct S<X,Y> implements Relay<Y>{map<V:Sendable>(seed:Y,value:V)->V{return value}}\n"
        "fn forward<A,T:Relay<A>,U:Sendable>(r:T,seed:A,value:U)->U{return r.map(seed,value)}\n"
        "export fn genericMethodNumber()->i64{return forward<i64,S<string,i64>,i64>(S<string,i64>{},0,41)}\n"
        "export fn genericMethodText()->string{return forward<i64,S<string,i64>,string>(S<string,i64>{},0,\"map\"+\"ped\")}\n");
    source_generic_requirement_run(request,
        "interface Apply{map<U,V>(value:U,body:fn(U)->U,tag:V)->U}\n"
        "struct S implements Apply{map<A,B>(value:A,body:fn(A)->A,tag:B)->A{return body(value)}}\n"
        "fn forward<T:Apply>(r:T)->i64{return r.map(40,fn(v){return v+1},\"tag\")}\n"
        "export fn genericMethodNumber()->i64{return forward<S>(S{})}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Left<A>{map<U>(value:Array<U>,tag:A)->U}\n"
        "interface Right<B>{map<V>(value:Array<V>,tag:B)->V}\n"
        "interface Both<C> extends Right<C>,Left<C>{}\n"
        "struct S implements Both<string>{map<W>(value:Array<W>,tag:string)->W{return value[0]}}\n"
        "fn forward<T:Both<string>>(r:T)->i64{return r.map([41],\"tag\")}\n"
        "export fn genericMethodNumber()->i64{return forward<S>(S{})}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "var trace=0\n"
        "interface Order{map<U>(first:U,second:U)->U}\n"
        "struct S implements Order{map<V>(first:V,second:V)->V{return first}}\n"
        "fn receiver<T>(r:T)->T{trace=trace*10+1;return r}\n"
        "fn stamp(n:i64)->i64{trace=trace*10+n;return n}\n"
        "fn forward<T:Order>(r:T)->i64{return receiver<T>(r).map(stamp(2),stamp(3))}\n"
        "export fn genericMethodNumber()->i64{const value=forward<S>(S{});if (trace==123){return 41};return 0}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
}
static void source_requirement_inference_negative(XrXirSourceRequest *request) {
    const char *rejected[] = {
        "interface I{map<U,V>(value:U)->U}\nfn bad<T:I>(r:T)->i64{return r.map(41)}\n",
        "interface I{map<U:Sendable>(value:U)->U}\nfn bad<T:I,A>(r:T,a:A)->A{return r.map(a)}\n",
        "interface I{map<U>(a:U,b:U)->U}\nfn bad<T:I>(r:T)->i64{return r.map(41,\"wrong\")}\n",
        "interface I<A>{map<U>(seed:A,value:U)->U}\nfn bad<T:I<string>>(r:T)->i64{return r.map(0,41)}\n",
        "interface I{map<U>(f:fn(U)->U)->i64}\nfn bad<T:I>(r:T)->i64{return r.map(fn(x){return x})}\n",
        "interface I{map<U,V>(value:U,tag:V)->U}\nfn bad<T:I>(r:T)->i64{return r.map<i64>(41,\"tag\")}\n",
        "interface Evidence{}\ninterface I{map<U:Evidence>(value:U)->U}\nfn bad<T:I>(r:T)->i64{return r.map(41)}\n"
    };
    const char *expected_messages[]={
        "cannot infer all method type arguments; supply an explicit list", /* own phantom V remains unresolved */
        "method type argument does not prove the declared constraint", /* original method Sendable obligation */
        "expression cannot satisfy its declared type", /* second value fails known i64 context */
        "expression cannot satisfy its declared type", /* original interface string prefix fixed */
        "closure parameter requires an annotation or complete callable context", /* lambda own type has no argument or result evidence */
        "interface method requires its exact explicit type arguments or an omitted list", /* partial explicit tuple rejected */
        "method type argument does not prove the declared constraint", /* scalar lacks explicit interface proof */
    };
    _Static_assert(sizeof(expected_messages)/sizeof(*expected_messages)==sizeof(rejected)/sizeof(*rejected), "Every rejection needs an exact diagnostic");
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        write_source(request->entry_path,rejected[i]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        if (status!=XR_XIR_BAD_TYPE) fprintf(stderr,"inference rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked);
        if (strcmp(diagnostic.message,expected_messages[i]))
            fprintf(stderr,"inference diagnostic %u: expected '%s', got '%s'\n",
                i,expected_messages[i],diagnostic.message);
        CHECK(strcmp(diagnostic.message,expected_messages[i])==0);
        if (result.snapshot) CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);
        xr_xir_source_result_free(&result);
    }
}
static void source_requirement_inference_cases(XrXirSourceRequest *request) {
    source_requirement_inference_positive(request); source_requirement_inference_negative(request);
}
#endif // XIR_SOURCE_REQUIREMENT_INFERENCE_CASES_H
