/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_method_where_cases.h - Method-owned conditional requirements
 *
 * KEY CONCEPT:
 *   Parent and method parameters retain their declaration identity through owned packets.
 */
#ifndef XIR_SOURCE_METHOD_WHERE_CASES_H
#define XIR_SOURCE_METHOD_WHERE_CASES_H
static void source_interface_method_where_positive(XrXirSourceRequest *request) {
    source_generic_requirement_run(request,
        "interface Mapper<A> { map<U:Sendable>(seed:A,value:U)->U where U:Sendable, U:Sendable }\n"
        "struct Adapter<X,Y> implements Mapper<X> { seed:X; label:Y; "
        "map<V>(seed:X,value:V)->V where V:Sendable{return value} }\n"
        "fn apply<A,T:Mapper<A>,U:Sendable>(mapper:T,seed:A,value:U)->U{return mapper.map<U>(seed,value)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64,Adapter<i64,string>,i64>(Adapter<i64,string>{seed:0,label:\"a\"},0,41)}\n"
        "export fn genericMethodText()->string{return apply<i64,Adapter<i64,string>,string>(Adapter<i64,string>{seed:0,label:\"b\"},0,\"map\"+\"ped\")}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> {}\n"
        "interface Strong<A> extends Evidence<A> {}\n"
        "interface Left<P> { map<U>(value:U)->U where U:Strong<P> }\n"
        "interface Right<Q> { map<V>(value:V)->V where V:Strong<Q> & Evidence<Q> }\n"
        "interface Both<R> extends Left<R>,Right<R> {}\n"
        "struct Token implements Strong<i64> { value:i64 }\n"
        "struct Adapter<X,Y> implements Both<X> { map<W:Strong<X>>(value:W)->W{return value} }\n"
        "fn apply<P,T:Both<P>,U:Strong<P>>(mapper:T,value:U)->U{return mapper.map<U>(value)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64,Adapter<i64,string>,Token>(Adapter<i64,string>{},Token{value:41}).value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> {}\n"
        "interface Forward { map<U,V>(value:U,tag:V)->U where U:Evidence<V> }\n"
        "struct Token implements Evidence<string> { value:i64 }\n"
        "struct Adapter<X,Y> implements Forward { map<C:Evidence<D>,D>(value:C,tag:D)->C{return value} }\n"
        "fn apply<T:Forward,A:Evidence<B>,B>(mapper:T,value:A,tag:B)->A{return mapper.map<A,B>(value,tag)}\n"
        "export fn genericMethodNumber()->i64{return apply<Adapter<i64,string>,Token,string>(Adapter<i64,string>{},Token{value:41},\"tag\").value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> { evidence()->A }\n"
        "interface Nested<A> { map<U>(value:U)->U where U:Evidence<Array<A>> }\n"
        "struct Token implements Evidence<Array<i64>> { value:i64; evidence()->Array<i64>{return [this.value]} }\n"
        "struct Adapter<X,Y> implements Nested<Y> { map<V:Evidence<Array<Y>>>(value:V)->V{return value} }\n"
        "fn apply<Z,T:Nested<Z>,U:Evidence<Array<Z>>>(mapper:T,value:U)->U{return mapper.map<U>(value)}\n"
        "export fn genericMethodNumber()->i64{return apply<i64,Adapter<string,i64>,Token>(Adapter<string,i64>{},Token{value:41}).value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
    source_generic_requirement_run(request,
        "interface Evidence<A> { evidence()->A }\n"
        "interface SelfMap { map<U>(value:U)->U where U:Evidence<U> }\n"
        "struct Token implements Evidence<Token> { value:i64; evidence()->Token{return this} }\n"
        "struct Adapter implements SelfMap { map<V>(value:V)->V where V:Evidence<V>{return value} }\n"
        "fn apply<T:SelfMap,U:Evidence<U>>(mapper:T,value:U)->U{return mapper.map<U>(value)}\n"
        "export fn genericMethodNumber()->i64{return apply<Adapter,Token>(Adapter{},Token{value:41}).value}\n"
        "export fn genericMethodText()->string{return \"mapped\"}\n");
}
static void source_interface_method_where_rejections(XrXirSourceRequest *request) {
    const struct { const char *source; XrXirStatus status; const char *reason; } rejected[] = {
        {("interface Evidence<A> {}\n"
          "interface I { map<U>(value:U)->U where U:Evidence<U> }\n"
          "fn unused<T:I,U>(mapper:T,value:U)->U{return mapper.map<U>(value)}\n"), XR_XIR_BAD_TYPE,"method type argument does not prove"},
        {"interface I<A> { map<U>(value:U)->U where A:Sendable }\n", XR_XIR_BAD_STRUCTURE,"module graph build failed"},
        {"interface I<A:Sendable> { map<U>(value:U)->U where A:Sendable }\n", XR_XIR_BAD_STRUCTURE,"module graph build failed"},
        {"interface I<A> { map(value:A)->A where A:Sendable }\n", XR_XIR_BAD_STRUCTURE,"module graph build failed"},
        {"interface I { map<U>(value:U)->U where Missing:Sendable }\n", XR_XIR_BAD_STRUCTURE,"module graph build failed"},
        {"interface I { map<U>(value:U)->U where U.Member:Sendable }\n", XR_XIR_BAD_STRUCTURE,"module graph build failed"},
        {("interface I { map<U>(value:U)->U where U:Sendable }\n"
          "fn unused<T:I,U>(receiver:T,value:U)->U{return receiver.map<U>(value)}\n"), XR_XIR_BAD_TYPE,"method type argument does not prove"},
        {("interface I { map<U>(value:U)->U }\n"
          "struct S implements I { map<V>(value:V)->V where V:Sendable{return value} }\n"), XR_XIR_BAD_TYPE,"implementation witness definition obligations failed"}
    };
    const char *syntax_reasons[] = {
        "'where' names 'A', which is not a type parameter here",
        "'where' names 'A', which is not a type parameter here",
        "'where' requires type parameters to constrain",
        "'where' names 'Missing', which is not a type parameter here",
        "expected ':' after type parameter in 'where'"
    };
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
        if (i >= 1 && i <= 5) source_query_syntax_boundary(request, rejected[i].source, syntax_reasons[i - 1]);
        write_source(request->entry_path,rejected[i].source);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_check(request, &result, &diagnostic, NULL);
        if (status!=rejected[i].status || !strstr(diagnostic.message,rejected[i].reason))
            fprintf(stderr,"method where rejection %u: %u %s\n",i,status,diagnostic.message);
        CHECK(status==rejected[i].status && !result.checked && !result.snapshot && strstr(diagnostic.message,rejected[i].reason));
        if (i >= 1 && i <= 5) source_query_syntax_graph_diagnostic(&diagnostic);
        CHECK(!result.snapshot);
        xr_xir_compile_source_result_free(&result);
    }
}
#endif // XIR_SOURCE_METHOD_WHERE_CASES_H
