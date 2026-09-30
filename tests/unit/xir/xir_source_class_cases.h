/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_class_cases.h - Source class identity and constructor obligations
 *
 * KEY CONCEPT:
 *   Definition checks precede every executable specialization, even for unused bodies.
 */
#ifndef XIR_SOURCE_CLASS_CASES_H
#define XIR_SOURCE_CLASS_CASES_H
#include "xir/xxir_class.h"
static void source_class_definition_cases(XrXirSourceRequest *request) {
    static const struct { const char *source; unsigned status; const char *message; } cases[]={
    {"final class Box<T>{value:T constructor(v:T){this.value=v} get()->T{return this.value}}",0,""},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused<T>(v:T)->Box<T>{return Box<T>(v)}",0,""},
    {"final class Container<T>{items:Array<T> constructor(v:Array<T>){this.items=v} get()->Array<T>{return this.items}}",0,""},
    {"final class Box<T>{value:T constructor(v:T){this.value=v} bad()->i64{return this.value.missing()}}",3,"receiver has no declared interface requirements"},
    {"final class Box<T>{private value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<i64>)->i64{return b.value}",3,"field access is not permitted"},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<Array<bool>>){}",3,"class field carrier is not implemented"},
    {"struct Pair{value:i64}\nfinal class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<Pair>){}",3,"class field carrier is not implemented"},
    {"final class Box<T>{const value:T constructor(v:T){this.value=v;this.value=v}}",4,"write may overwrite already initialized const storage"},
    {"final class Box<T>{value:T constructor(v:T){}}",4,"read requires storage initialized on every incoming path"},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<Slice<u8>>){}",3,"nominal type requires its exact explicit arguments"},
    {"final class Box<T>{value:T constructor(v:T){this.value=v}}\nfn unused(b:Box<MutSlice<u8>>){}",3,"nominal type requires its exact explicit arguments"},
    {"class C {x:i64 constructor(){this.x=1}}",3,"class execution currently requires an explicit final root declaration"},
    {"final class C{x:i64}",3,"class execution currently requires one explicit complete constructor"},
    {"final class C{x:Array<i64> constructor(){this.x=[1]}}",0,""},
    {"final class C{x:i64=1 constructor(){}}",3,"class field requires an explicit admitted constructor value"},
    {"final class C{x:Array<string> constructor(x:Array<string>){this.x=x} get()->Array<string>{return this.x}}",0,""},
    {"final class C{x:Array<Array<i64>> constructor(x:Array<Array<i64>>){this.x=x}}",3,"class field carrier is not implemented"},
    {"final class C<T>{x:T constructor(x:T){this.x=x}}",0,""},
    {"final class C{x:Array<i64> constructor(){}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{const x:Array<i64> constructor(){this.x=[1];this.x=[2]}}",4,"write may overwrite already initialized const storage"},
    {"final class C{private x:Array<i64> constructor(){this.x=[1]}}\nfn unused(c:C)->Array<i64>{return c.x}",3,"field access is not permitted"},
    {"final class C{const x:Array<i64> constructor(){this.x=[1]} bad(){this.x=[2]}}",3,"field access is not permitted"},
    {"final class C{x:Array<i64> constructor(){this.x=[1]} bad(){this.x[0]=2}}",3,"value mutation requires a mutable named root"},
    {"final class C{x:Array<i64> constructor(){this.x=[1]} bad(){this.x.push(2)}}",3,"value mutation requires a mutable named root"},
    {"final class Array<T>{constructor(){}};final class C{x:Array<i64> constructor(x:Array<i64>){this.x=x}}",3,"class field carrier is not implemented"},
    {"final class C{private x:i64 constructor(){this.x=1}}\nfn unused(c:C)->i64{return c.x}",3,"field access is not permitted"},
    {"final class C{const x:i64 constructor(){this.x=1} bad(){this.x=2}}",3,"field access is not permitted"},
    {"final class C{x:i64 constructor(){}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{const x:i64 constructor(b:bool){if(b){this.x=1}}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{const x:i64 constructor(){this.x=1;this.x=2}}",4,"write may overwrite already initialized const storage"},
    {"final class C{x:i64 constructor(){const n=this.x;this.x=n}}",4,"read requires storage initialized on every incoming path"},
    {"final class C{x:i64 constructor(){this.x=1;const n=this}}",3,"class this cannot escape before constructor completion"},
    {"final class C{x:i64 constructor(){this.x=1;const cb=fn()->i64{return this.x}}}",3,"class this cannot be captured during construction"},
    {"final class C{x:i64 constructor(){this.x=1;defer {this.x=2}}}",3,"class constructor cleanup cannot capture unpublished this"},
    {"final class C{const x:i64 constructor(b:bool){if(b){this.x=1}else{this.x=2}}}",0,""},
    {"final class C{x:i64 constructor(){this.x=1} static bad()->i64{return this.x}}",4,"name is not an initialized value"},
    {"final class C{x:i64 private constructor(){this.x=1}}\nfn bad()->C{return C()}",3,"constructor arity or authority mismatch"},
    {"final class C<T>{x:i64 constructor(x:i64){this.x=x} get()->i64{return this.x}}\nfn read()->i64{return C<string>(41).get()}",0,""},
    {"final class C{x:i64 constructor(){this.x=1;Coro.yield()}}",3,"declared no_suspend function may suspend or call an unqualified callable"},
    {"final class C{constructor(){}}\nfn bad(c:C){c=C()}",3,"assignment requires a mutable binding"},
    {"final class C{constructor(){}}\nfn missing<T>(value:T)->i64{return value.get()}",3,"receiver has no declared interface requirements"},
    {"final class C{x:i64 constructor(){this.x=1}}\nfn send<T:Sendable>(x:T){}\nfn bad(){send<C>(C())}",3,"type argument does not prove the declared constraint"},
    {"final class Array<T>{constructor(){}}\nfn typed(x:Array<i64>){}",0,""},
    {"final class Array<T>{constructor(){}}\nfn wrong(x:Array<i64>)->i64{return x.len()}",3,"unknown struct field"},
    };
    for (uint32_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        write_source(request->entry_path,cases[i].source);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        if ((unsigned)status!=cases[i].status || strcmp(diagnostic.message,cases[i].message))
            fprintf(stderr,"class case %u: %u %s\n",i,status,diagnostic.message);
        CHECK((unsigned)status==cases[i].status);
        CHECK(!strcmp(diagnostic.message,cases[i].message));
        CHECK((result.checked!=NULL)==(status==XR_XIR_OK));
        if (status==XR_XIR_OK) CHECK(result.snapshot!=NULL);
        xr_xir_source_result_free(&result);
    }
    const char *root=request->authority->physical_root;
    size_t capacity=strlen(root)+32;char *library=malloc(capacity);CHECK(library);
    CHECK(snprintf(library,capacity,"%s/class_private.xr",root)>0);
    write_source(library,"export final class Hidden{private n:i64 private constructor(){this.n=41} static make()->Hidden{return Hidden()}}\n");
    const char *cross[]={
        "import {Hidden} from \"./class_private\"\nfn unused(x:Hidden)->i64{return x.n}\n",
        "import {Hidden} from \"./class_private\"\nfn unused()->Hidden{return Hidden()}\n"
    };
    const char *messages[]={"field access is not permitted","constructor arity or authority mismatch"};
    for (uint32_t i=0;i<2;++i) {
        write_source(request->entry_path,cross[i]);
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        CHECK(xr_xir_source_check(request,&result,&diagnostic)==XR_XIR_BAD_TYPE && !result.checked);
        CHECK(!strcmp(diagnostic.message,messages[i]));xr_xir_source_result_free(&result);
    }
    CHECK(!remove(library));free(library);
}
#endif // XIR_SOURCE_CLASS_CASES_H
