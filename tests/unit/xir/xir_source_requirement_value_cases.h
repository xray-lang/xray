/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_requirement_value_cases.h - Definition-bound owned method values
 *
 * KEY CONCEPT:
 *   Explicit binding retains receiver ownership and original generic authority.
 */
#ifndef XIR_SOURCE_REQUIREMENT_VALUE_CASES_H
#define XIR_SOURCE_REQUIREMENT_VALUE_CASES_H
/* Include after xir_source_generic_requirement_cases.h to reuse its real
 * Checked packet, specialization, VM and retained-result ownership consumer.
 */
static void source_requirement_value_positive(XrXirSourceRequest *request) {
    source_generic_requirement_run(request,
        "interface I<A>{map<U:Sendable>(seed:A,value:U)->U}\n"
        "struct Adapter<X,Y> implements I<Y>{map<V:Sendable>(seed:Y,value:V)->V{return value}}\n"
        "fn bind<A,T:I<A>,K,U:Sendable>(receiver:T,unused:K)->fn(A,U)->U{return receiver.map<U>}\n"
        "export fn genericMethodNumber()->i64{const saved=bind<i64,Adapter<string,i64>,bool,i64>(Adapter<string,i64>{},true);return saved(0,41)}\n"
        "export fn genericMethodText()->string{const saved=bind<i64,Adapter<string,i64>,bool,string>(Adapter<string,i64>{},true);return saved(0,\"map\"+\"ped\")}\n");
    source_generic_requirement_run(request,
        "interface I{map(value:i64)->i64}\n"
        "struct Adapter implements I{value:i64=0;map(value:i64)->i64{return value+this.value}}\n"
        "const visits=Atomic(0)\n"
        "fn touch<T:I>(receiver:T)->T{visits.fetchAdd(1);return receiver}\n"
        "fn bind<T:I>(receiver:T)->fn(i64)->i64{const saved=touch<T>(receiver).map;return saved}\n"
        "export fn genericMethodNumber()->i64{const saved=bind<Adapter>(Adapter());const first=saved(40);const second=saved(0);return first+second+visits.load()}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface I{map<U:Sendable>(value:U)->U}\n"
        "struct Adapter implements I{map<V:Sendable>(value:V)->V{return value}}\n"
        "fn lifted<T:I>(receiver:T)->fn(i64)->i64{const saved=receiver.map<i64>;defer {const ignored=1;};return fn(value:i64)->i64{return saved(value)}}\n"
        "export fn genericMethodNumber()->i64{const saved=lifted<Adapter>(Adapter{});return saved(41)}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A>{}\n"
        "interface Strong<A> extends Evidence<A>{}\n"
        "interface Left<A>{map<U:Strong<A>>(value:U)->U}\n"
        "interface Right<A>{map<V:Strong<A>&Evidence<A>>(value:V)->V}\n"
        "interface Both<A> extends Left<A>,Right<A>{}\n"
        "struct Token implements Strong<i64>{value:i64}\n"
        "struct Adapter<X,Y> implements Both<Y>{map<W:Strong<Y>>(value:W)->W{return value}}\n"
        "fn bind<A,T:Both<A>,U:Strong<A>>(receiver:T)->fn(U)->U{return receiver.map<U>}\n"
        "export fn genericMethodNumber()->i64{const saved=bind<i64,Adapter<string,i64>,Token>(Adapter<string,i64>{});return saved(Token{value:41}).value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
}
static void source_requirement_value_negative(XrXirSourceRequest *request) {
    const struct {const char *source,*reason;} rejected[] = {
        {("interface I{map<U:Sendable>(value:U)->U}\n"
          "fn unused<T:I,U>(receiver:T)->fn(U)->U{return receiver.map<U>}\n"),
            "method type argument does not prove"},
        {("interface I{map<U>(value:U)->U}\n"
          "fn unused<T:I>(receiver:T)->fn(i64)->i64{return receiver.map}\n"),
            "interface method value requires its complete explicit type arguments"},
        {("interface I{map<U>(value:U)->U}\n"
          "fn unused<T:I>(receiver:T)->fn(i64)->i64{return receiver.map<i64,string>}\n"),
            "interface method value requires its complete explicit type arguments"},
        {("interface I{}\n"
          "fn unused<T:I>(receiver:T)->fn()->i64{return receiver.extra}\n"),
            "member is absent from declared interface requirements"},
        {("interface I{map()->i64}\n"
          "fn unused<T:I>(receiver:T)->fn()->i64{return receiver.map<i64>}\n"),
            "interface method value requires its complete explicit type arguments"},
        {("interface I{map<U>(value:U)->U}\n"
          "fn unused<T:I>(receiver:T)->fn(string)->string{return receiver.map<i64>}\n"),
            "callable conversion may only weaken its outer execution contract"}
    };
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        write_source(request->entry_path,rejected[i].source);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if (status!=XR_XIR_BAD_TYPE || !strstr(diagnostic.message,rejected[i].reason))
            fprintf(stderr,"requirement value rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !result.snapshot && strstr(diagnostic.message,rejected[i].reason));
        CHECK(!result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
}
#endif // XIR_SOURCE_REQUIREMENT_VALUE_CASES_H
