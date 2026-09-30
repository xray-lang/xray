/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_dependency_ready_cases.h - Ordered evidence and single runtime evaluation
 *
 * KEY CONCEPT:
 *   Later ready type evidence guides earlier contextual syntax; runtime values
 *   retain source order and survive producer destruction through Checked packets.
 */
static void source_dependency_ready_cases(XrXirSourceRequest *request) {
        source_generic_requirement_run(request,
            "final class Counter{n:i64;label:string;constructor(n:i64){this.n=n;this.label=\"mapped\"} bump(){this.n+=1} get()->i64{return this.n}}\n"
            "export fn genericMethodNumber()->i64{const c=Counter(40);const alias=c;alias.bump();return c.get()}\n"
            "export fn genericMethodText()->string{const c=Counter(0);return c.label}\n");
        source_generic_requirement_run(request,
            "fn pair<T>(a:T,b:T)->T{return b}\n"
            "export fn genericMethodNumber()->i64{const narrow:i8=41;const result:i8=pair(1,narrow);return result}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn choose<T>(f:fn(T)->T,seed:T)->T{return f(seed)}\n"
            "export fn genericMethodNumber()->i64{const seed:i8=40;const result:i8=choose(fn(v){return v+1},seed);return result}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn choose<T>(empty:Array<T>,seed:T)->T{return seed}\n"
            "export fn genericMethodNumber()->i64{const seed:i8=41;return choose([],seed)}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "var trace=0\nfn first()->i64{trace=trace*10+1;return 0}\nfn second()->i8{trace=trace*10+2;return 41}\n"
            "fn choose<T>(a:T,b:T)->T{return b}\n"
            "export fn genericMethodNumber()->i64{const result=choose(first(),second());if(trace==12){return result};return 0}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn choose<T>(f:fn(T)->T,seed:T)->T{return f(seed)}\n"
            "fn apply<A>(seed:A)->A{return choose(fn(v){return v},seed)}\n"
            "export fn genericMethodNumber()->i64{return apply(41)}\n"
            "export fn genericMethodText()->string{return apply(\"mapped\")}\n");
        source_generic_requirement_run(request,
            "fn choose<T>(f:fn(T)->T,seed:T)->T{return f(seed)}\n"
            "export fn genericMethodNumber()->i64{const result=choose(fn(v){return v+1},40);return result}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "interface Apply{map<U>(f:fn(U)->U,value:U)->U}\n"
            "struct S implements Apply{map<V>(f:fn(V)->V,value:V)->V{return f(value)}}\n"
            "fn forward<T:Apply>(r:T)->i64{const n:i8=40;return r.map(fn(v){return v+1},n)}\n"
            "fn text<T:Apply>(r:T)->string{return r.map(fn(v){return v},\"mapped\")}\n"
            "export fn genericMethodNumber()->i64{return forward(S{})}\n"
            "export fn genericMethodText()->string{return text(S{})}\n");
        source_generic_requirement_run(request,
            "struct S<X>{map<U>(f:fn(U)->U,value:U)->U{return f(value)} static choose<V>(f:fn(V)->V,value:V)->V{return f(value)}}\n"
            "export fn genericMethodNumber()->i64{const n:i8=40;return S<string>{}.map(fn(v){return v+1},n)}\n"
            "export fn genericMethodText()->string{return S<i64>.choose(fn(v){return v},\"mapped\")}\n");
        source_generic_requirement_run(request,
            "fn pair<T>(a:T,b:T)->T{return b}\n"
            "export fn genericMethodNumber()->i64{const n:i8=40;const value=pair(0,n+1);const narrow:i8=value;return narrow}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn pair<T>(a:T,b:T)->T{return b}\n"
            "export fn genericMethodNumber()->i64{const n:i8=41;const narrow:i8=pair<i8>((1+2)+3,n);return narrow}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn choose<T>(f:fn(T)->T,value:T)->T{return f(value)}\n"
            "export fn genericMethodNumber()->i64{const result=choose(fn(v){return v},(40+1));return result}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn id<T>(v:T)->T{return v}\n"
            "export fn genericMethodNumber()->i64{const n:i8=id(true?(1+2)+38:40+1);return n}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn empty<T>()->Array<T>{return []}\n"
            "export fn genericMethodNumber()->i64{const a:Array<i64>=true?empty():empty();const v:f64=true?1:2.5;if(v==1.0){return 41};return 0}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "fn choose<T>(f:fn(T)->T,seed:T)->T{return f(seed)}\n"
            "export fn genericMethodNumber()->i64{const n:i8=40;const result=choose(fn(v){return v+1},match(true){true->n,false->n});const narrow:i8=result;return narrow}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        source_generic_requirement_run(request,
            "enum Item{Left{value:i8},Right{value:i8}}\n"
            "fn choose<T>(f:fn(T)->T,seed:T)->T{return f(seed)}\n"
            "export fn genericMethodNumber()->i64{const item=Item.Right{value:40};const value:i64=999;const result=choose(fn(v){return v+1},match(item){Item.Left{value}->value,Item.Right{value}->value});const narrow:i8=result;return narrow}\n"
            "export fn genericMethodText()->string{return \"mapped\"}\n");
        const char *negative[] = {
            "fn pair<T>(a:T,b:T)->T{return b}\nfn bad()->i64{const a:i8=1;const b:i64=41;return pair(a,b)}\n",
            "fn pair<T>(a:T,b:T)->T{return b}\nfn bad(){const a:Array<i64>=[1];const b:Array<i8>=[2];const c=pair(a,b)}\n",
            "fn id<T:Sendable>(x:T)->T{return x}\nfn bad<A>(x:A)->A{return id(x)}\n",
            "fn ghost<T,U>(x:T)->T{return x}\nfn bad()->i64{const x:i8=41;return ghost(x)}\n",
            "fn pair<T>(a:T,b:T)->T{return a}\nfn bad(){const x=pair(1,2.5)}\n",
            "fn bad(){const x=true?1:2.5}\n",
            "fn bad(){const a:i8=41;const b:i64=41;const x=true?a:b;const n:i8=x}\n"
        };
        const char *messages[] = {
            "expression cannot satisfy its declared type",
            "expression cannot satisfy its declared type",
            "type argument does not prove the declared constraint",
            "cannot infer all declaration type arguments; supply an explicit list",
            "expression cannot satisfy its declared type",
            "conditional requires a common admitted type",
            "expression cannot satisfy its declared type"
        };
        for (uint32_t n=0;n<sizeof(negative)/sizeof(*negative);++n) {
            write_source(request->entry_path,negative[n]);
            XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
            XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
            if (status!=XR_XIR_BAD_TYPE || strcmp(messages[n],diagnostic.message)) fprintf(stderr,"ready negative %u %u %s\n",n,status,diagnostic.message);
            CHECK(status==XR_XIR_BAD_TYPE && !result.checked && !strcmp(messages[n],diagnostic.message));
            if(result.snapshot) CHECK(!xr_xir_source_snapshot_view(result.snapshot)->complete);
            xr_xir_source_result_free(&result);
        }
}
