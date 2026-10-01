/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_admission.c - Reject invalid source without a partial artifact
 *
 * KEY CONCEPT:
 *   Reusing a compiler session must not retain declarations from rejected input.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include "module/xmodule_resolver.h"
#include "base/xmalloc.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static void write_source(const char *path, const char *source) {
    FILE *file = fopen(path, "wb"); CHECK(file);
    size_t length = strlen(source);
    CHECK(fwrite(source, 1, length, file) == length && fclose(file) == 0);
}
#include "xir_source_catch_cases.h"
#include "xir_source_panic_cases.h"
#include "xir_source_cleanup_admission.h"
#include "xir_source_conditional_cases.h"
static void stdlib_resolution(void) {
    XrModuleResolverConfig config = {XR_SOURCE_STDLIB, NULL, NULL, 0};
    XrModuleResolver *resolver = xr_module_resolver_new(&config); CHECK(resolver);
    XrModuleId first = {0}, second = {0}; char *error = NULL;
    CHECK(xr_module_resolver_resolve(resolver, "std/io/output", NULL, NULL, &first, &error) == 0 && !error);
    CHECK(first.kind == XR_MOD_STDLIB && first.authority.kind == XR_MODULE_IDENTITY_STDLIB);
    CHECK(!strcmp(first.authority.namespace_id, "io") && !strcmp(first.logical_path, "io/output.xr"));
    CHECK(xr_module_identity_valid(first.canonical, NULL));
    CHECK(xr_module_resolver_resolve(resolver, "std/io/output", NULL, NULL, &second, &error) == 0 && !error);
    CHECK(!strcmp(first.canonical, second.canonical) && first.canonical != second.canonical);
    xr_module_id_cleanup(&first); xr_module_id_cleanup(&second);
    const char *invalid[] = {"std/io", "std/io/io", "std/io/../io/output", "std/io//output", "std/io/output.xr",
        "std/io/output/", "std/io/output/index", "std/io/output:bad", "std/io/0output", "std/unknown/output"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(xr_module_resolver_resolve(resolver, invalid[i], NULL, NULL, &first, &error) != 0);
        CHECK(!first.canonical && !first.source_path && !first.authority.physical_root);
        xr_free(error); error = NULL;
    }
    xr_module_resolver_free(resolver);
    config.stdlib_path = NULL; resolver = xr_module_resolver_new(&config); CHECK(resolver);
    CHECK(xr_module_resolver_resolve(resolver, "std/io/output", NULL, NULL, &first, &error) != 0);
    CHECK(error && !first.canonical); xr_free(error); xr_module_resolver_free(resolver);
}
static void primitive_authority(XrCompilerSession *session, const char *directory) {
    char io[XR_TEST_PATH_MAX], output[XR_TEST_PATH_MAX], other[XR_TEST_PATH_MAX];
    CHECK(snprintf(io, sizeof(io), "%s/io", directory) > 0 && xr_test_mkdir(io) == 0);
    CHECK(snprintf(output, sizeof(output), "%s/output.xr", io) > 0);
    CHECK(snprintf(other, sizeof(other), "%s/other.xr", io) > 0);
    const char *body = "export fn emit(value: string) -> bool { return __writeStderr(value) }\n";
    write_source(output, body); write_source(other, body);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, directory};
    XrXirSourceRequest request = {session, output, &authority, NULL, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *artifact = NULL;
    XrXirSourceResult query_result_1 = {0};
    XrXirStatus query_status_1 = xr_xir_source_check(&request, &query_result_1, NULL);
    artifact = query_result_1.checked; query_result_1.checked = NULL;
    xr_xir_source_result_free(&query_result_1);
    CHECK(query_status_1 != XR_XIR_OK && !artifact);
    authority.kind = XR_MODULE_IDENTITY_STDLIB; authority.namespace_id = "math";
    XrXirSourceResult query_result_2 = {0};
    XrXirStatus query_status_2 = xr_xir_source_check(&request, &query_result_2, NULL);
    artifact = query_result_2.checked; query_result_2.checked = NULL;
    xr_xir_source_result_free(&query_result_2);
    CHECK(query_status_2 != XR_XIR_OK && !artifact);
    authority.namespace_id = "io"; request.entry_path = other;
    XrXirSourceResult query_result_3 = {0};
    XrXirStatus query_status_3 = xr_xir_source_check(&request, &query_result_3, NULL);
    artifact = query_result_3.checked; query_result_3.checked = NULL;
    xr_xir_source_result_free(&query_result_3);
    CHECK(query_status_3 != XR_XIR_OK && !artifact);
    request.entry_path = output;
    XrXirSourceResult query_result_4 = {0};
    XrXirStatus query_status_4 = xr_xir_source_check(&request, &query_result_4, NULL);
    artifact = query_result_4.checked; query_result_4.checked = NULL;
    xr_xir_source_result_free(&query_result_4);
    CHECK(query_status_4 == XR_XIR_OK && artifact);
    xr_xir_artifact_free(artifact); artifact = NULL;
    write_source(output, "export fn emit() -> bool { return __writeStderr(1) }\n");
    XrXirSourceResult query_result_5 = {0};
    XrXirStatus query_status_5 = xr_xir_source_check(&request, &query_result_5, NULL);
    artifact = query_result_5.checked; query_result_5.checked = NULL;
    xr_xir_source_result_free(&query_result_5);
    CHECK(query_status_5 != XR_XIR_OK && !artifact);
    write_source(output, "export fn emit() -> bool { return __writeStderr() }\n");
    XrXirSourceResult query_result_6 = {0};
    XrXirStatus query_status_6 = xr_xir_source_check(&request, &query_result_6, NULL);
    artifact = query_result_6.checked; query_result_6.checked = NULL;
    xr_xir_source_result_free(&query_result_6);
    CHECK(query_status_6 != XR_XIR_OK && !artifact);
    write_source(output, "fn __writeStderr(value: i64) -> i64 { return value }\nprint(__writeStderr(7))\n");
    XrXirSourceResult query_result_7 = {0};
    XrXirStatus query_status_7 = xr_xir_source_check(&request, &query_result_7, NULL);
    artifact = query_result_7.checked; query_result_7.checked = NULL;
    xr_xir_source_result_free(&query_result_7);
    CHECK(query_status_7 == XR_XIR_OK && artifact);
    xr_xir_artifact_free(artifact);
    CHECK(xr_test_unlink(output) == 0 && xr_test_unlink(other) == 0 && xr_test_rmdir(io) == 0);
}
static void shadowed_coro(const XrXirSourceRequest *request, const char *root) {
    write_source(root, "import \"./lib\" as Coro\nprint(Coro.visible())\n");
    XrXirArtifact *artifact = NULL;
    XrXirSourceResult query_result_8 = {0};
    XrXirStatus query_status_8 = xr_xir_source_check(request, &query_result_8, NULL);
    artifact = query_result_8.checked; query_result_8.checked = NULL;
    xr_xir_source_result_free(&query_result_8);
    CHECK(query_status_8 == XR_XIR_OK && artifact);
    const XrXirModule *module = xr_xir_artifact_module(artifact);
    for (uint32_t f = 0; f < module->function_count; ++f)
        for (uint32_t i = 0; i < module->functions[f].instruction_count; ++i)
            CHECK(module->functions[f].instructions[i].op != XR_XIR_SUSPEND);
    xr_xir_artifact_free(artifact);
}
static const char *const rejected[] = {
    "var a=[1]\na[0]+=2\n",
    "var a=[[1]]\na[0][0]+=2\n",
    "fn raise<E:Error>(value:E){throw value}\nraise<i64>(1)\n",
    "fn raise<E:Error>(value:E){throw value}\nfn bad<T>(value:T){raise<T>(value)}\n",
    "fn bad<E:Error>(value:E)->i64{return value+1}\n",
    "struct S{value:i64}\nfn raise<E:Error>(value:E){throw value}\nraise<S>(S{value:1})\n",
    "struct Box<E:Error>{value:E}\nconst bad=Box<string>{value:\"wrong\"}\n",
    "struct Box<E:Error>{value:E}\nfn bad<T>(value:T){const box=Box<T>{value:value}}\n",
    "fn bad<Error>(value:Error){throw value}\n",
    "fn bad<E:Error & Unknown>(value:E){throw value}\n",
    "fn bad<E:Error<string>>(value:E){throw value}\n",
    "fn bad()->Error{return 1}\n",
    "fn bad<T>(value:T)->Error{return value}\n",
    "struct S{value:i64}\nfn bad(value:S)->Error{return value}\n",
    "fn bad(value:Error)->i64{return value.ordinal}\n",
    "fn bad(value:Error)->i64{return value+1}\n",
    "fn consume<T:Sendable>(value:T){}\nfn bad(value:Error){consume<Error>(value)}\n",
    "throw 91\n",
    "fn unused(){throw \"error\"}\n",
    "fn unused<T>(value:T){throw value}\n",
    "enum E{Bad{message:string}}\nfn unused(){throw E.Bad{message:1}}\n",
    "enum E<T:Sendable>{Bad{value:T}}\nfn unused<T>(value:T){throw E<T>.Bad{value:value}}\n",
    "enum E{Bad}\nfn unused(){if(false){throw 1}else{throw E.Bad}}\n",
    "fn f(v:f32)->i64{return match(v){_,16777217->1}}\n",
    "enum E{A{x:f32}}\nfn f(v:E)->i64{return match(v){E.A{},E.A{x:16777217}->1}}\n",
    "enum E{A{x:i64},B{x:string}}\nfn f(v:E)->i64{return match(v){E.A{x},E.B{x}->1}}\n",
    "enum E{A{x:i64},B}\nfn f(v:E)->i64{return match(v){E.A{x},E.B->1}}\n",
    "enum E{A,B{x:i64}}\nfn f(v:E)->i64{return match(v){E.A,E.B{x}->1}}\n",
    "enum E{A{x:i64,y:i64},B{x:i64}}\nfn f(v:E)->i64{return match(v){E.B{x},E.A{x,y:x}->x}}\n",
    "enum E{A,B}\nfn f(v:E)->i64{return match(v){E.A,E.B if(true)->1}}\n",
    "enum E{A{x:i8}}\nfn f(v:E)->i64{return match(v){E.A{x:-128..127}->1}}\n",
    "enum E{A{x:i8,b:bool}}\nfn f(v:E)->i64{return match(v){E.A{x:-128..0,b:true}->1,E.A{x:0..=127,b:false}->2}}\n",
    "enum E{A{x:u8}}\nfn f(v:E)->i64{return match(v){E.A{x:0..256}->1,E.A{}->2}}\n",
    "enum E{A{x:f32}}\nfn f(v:E)->i64{return match(v){E.A{x:0..1}->1,E.A{}->2}}\n",
    "enum E{A{x:f32}}\nfn f(v:E)->i64{return match(v){E.A{x:16777217}->1,E.A{}->2}}\n",
    "enum E{A{x:i8}}\nfn f(v:E)->i64{return match(v){E.A{x:-128..=127} if(true)->1}}\n",

    "enum E{A{x:i8}}\nfn f(v:E)->i64{return match(v){E.A{x:128}->1,E.A{}->2}}\n",
    "enum E{A{x:u64}}\nfn f(v:E)->i64{return match(v){E.A{x:-1}->1,E.A{}->2}}\n",
    "enum E{A{x:string}}\nfn f(v:E)->i64{return match(v){E.A{x:1}->1,E.A{}->2}}\n",
    "enum E<T>{A{x:T}}\nfn f<T>(v:E<T>)->i64{return match(v){E.A{x:1}->1,E.A{}->2}}\n",
    "enum E{A{x:i64}}\nfn f(v:E)->i64{return match(v){E.A{x:1}->1}}\n",

    "enum E{A,B}\nfn f(v:E)->i64{match(v){E.A->{break},E.B->{return 0}}}\n",
    "enum E{A,B}\nfn f(v:E)->i64{match(v){E.A->{return 1\nprint(2)},E.B->{return 0}}}\n",

    "enum E { A }\nconst v=match(E.A){_ if(1)->1,_ ->2}\n",
    "enum E { A{x:i64} }\nconst v=match(E.A{x:1}){E.A{x}->{x=2\nx}}\n",

    "enum E { A }\nconst v=match(E.A){E.A->{42\nprint(1)}}\n",
    "enum E { A }\nconst v=match(E.A){E.A->{if(true){42}\n1}}\n",
    "enum E { A }\nconst v=match(E.A){E.A->{const f=fn()->i64{42}\n1}}\n",

    "enum E { A, B }\nconst v=match(E.A){E.A->1}\n",
    "enum E { A, B }\nconst v=match(E.A){_ if(false)->1}\n",
    "enum E { A, B }\nconst v=match(E.A){E.A if(false)->1,E.B->2}\n",
    "enum I{A,B}\nenum O{Wrap{inner:I}}\nconst x=match(O.Wrap{inner:I.A}){O.Wrap{inner:I.A}->1}\n",
    "enum E{A{x:bool,y:bool}}\nconst n=match(E.A{x:true,y:false}){E.A{x:true,y:true}->1,E.A{x:false,y:false}->2}\n",
    "enum E{A{x:bool}}\nconst n=match(E.A{x:true}){E.A{x:true} if(false)->1,E.A{x:false}->2}\n",
    "enum I{A{x:i64}}\nenum O{Wrap{inner:I,x:i64}}\nconst n=match(O.Wrap{inner:I.A{x:1},x:2}){O.Wrap{inner:I.A{x},x}->x}\n",
    "enum E{A{x:i64}}\nconst n=match(E.A{x:1}){E.A{x:true}->1,_ ->2}\n",

    "enum E { A { x:i64 }, B }\nconst v=match(E.B){E.A{x,x}->1,E.B->2}\n",
    "enum E { A { x:i64 }, B }\nconst v=match(E.B){E.A{y}->1,E.B->2}\n",
    "enum E { A { x:i64 }, B }\nconst v=match(E.B){E.A{x}->x,E.B->x}\n",
    "enum E { A, B }\nenum F { A, B }\nconst v=match(E.A){F.A->1,E.B->2}\n",

    "enum E { A, B }\nconst e=E()\n",
    "enum E { A, B }\nvar e:E\n",
    "enum E { A { x:i64, y:string } }\nconst e=E.A { x:1 }\n",
    "enum E { A { x:i64, y:i64 } }\nconst e=E.A { x:1, x:2 }\n",
    "enum E { A { x:i64 } }\nconst e=E.A { x:\"bad\" }\n",
    "enum E { A { x:i64 } }\nconst e=E.A\n",
    "enum E { A }\nconst e=E.Missing\n",
    "enum E<T:Sendable> { A { x:T } }\nfn unused<T>(v:T)->E<T>{return E<T>.A { x:v }}\n",
    "enum E { A { x:Missing } }\n",

    "\"a\".contains(1)\n",
    "\"a\".contains()\n",
    "\"a\".endsWith(\"a\",\"b\")\n",
    "\"a\".startsWith<i64>(\"a\")\n",
    "fn unused<T>(x:T)->bool { return x.contains(\"a\") }\n",
    "fn unused<T>(x:T)->bool { return \"a\".endsWith(x) }\n",
    "\"a\".indexOf(1)\n",
    "\"a\".indexOf(\"a\",true)\n",
    "\"a\".lastIndexOf(\"a\",0)\n",
    "fn unused<T>(x:T)->i64{return x.indexOf(\"a\")}\n",

    "fn bad<T>(value:T)->i64{return len(value)}\n",
    "fn bad<T>(value:T)->bool{return value==\"x\"}\n",
    "print(\"x\"==1)\n",
    "print(len(1))\n",
    "print(len<string>(\"x\"))\n",

    "struct C<T>{static f<T>(value:T)->T{return value}}\n",
    "struct C{static f<T,T>(value:T)->T{return value}}\n",
    "struct C<T>{static f<U>(value:T)->U{return value}}\n",
    "struct C<T>{static f<U>(value:U=1)->U{return value}}\n",
    "struct C<T>{static f<U,V>(value:U)->U{return value}}\nC<i64>.f(1)\n",
    "struct C<T>{static f<U>(value:U)->U{return value}}\nC.f<i64,string>(\"bad\")\n",
    "struct C<T>{value:T\nf<U>(value:U)->U{return value}}\nconst c=C<i64>{value:1}\nconst f=c.f\n",
    "fn require<T:Sendable>(value:T)->T{return value}\nstruct C<T>{static f<U>(value:U)->U{return require<U>(value)}}\n",
    "struct C{static f<U:Sendable>(value:U)->U{return value}}\nC.f<fn()->i64>(fn()->i64{return 1})\n",
    "struct C{const value:i64\nconstructor(){this.value=1\nconst f=this.value<i64>\n}}\n",

    "struct C { static value()->i64 { return this.x } }\n",
    "struct C { static value(x:i64=this.x)->i64 { return x } }\n",
    "struct C { static value()->i64 { return 1 } }\nconst c=C()\nc.value()\n",
    "struct C { static value()->i64 { return 1 } }\nconst c=C()\nconst f=c.value\n",
    "struct C { value()->i64 { return 1 } }\nC.value()\n",
    "struct C { value()->i64 { return 1 } }\nconst f=C.value\n",
    "struct C { private static value()->i64 { return 1 } }\nC.value()\n",
    "struct C { private static value()->i64 { return 1 } }\nconst f=C.value\n",
    "struct C { static value(x:i64=7)->i64 { return x } }\nconst f=C.value\nf()\n",
    "struct C<T> { static value()->T { return 1 } }\n",
    "struct C<T> { static value(x:T=1)->T { return x } }\n",
    "struct C<T> { static value(x:T)->T { return x } }\nC.value<i64>(1)\n",
    "struct C { static value()->i64 { return 1 } }\nfn bad(){const C=7\nC.value()}\n",
    "struct C { static value(x:i64=\"bad\")->i64 { return x } }\n",

    "fn value(x:i64=caller)->i64 { return x }\nfn use()->i64 { const caller=7; return value() }\n",
    "struct C { const value:i64\n private constructor(value:i64=7) { this.value=value } }\nconst c=C()\n",
    "struct C { value:i64=7\n private get(x:i64=7)->i64 { return x } }\nprint(C().get())\n",
    "struct C { value:i64=7\n get(x:i64=7)->i64 { return x } }\nconst get=C().get\nprint(get())\n",
    "fn f(x:i64=\"bad\")->i64 { return x }\n",
    "fn f(a:i64=1,b:i64)->i64 { return b }\n",
    "fn f(a:i64,b:i64=a)->i64 { return b }\n",
    "fn f<T>(x:T=1)->T { return x }\n",
    "fn f(x:i64=1)->i64 { return x }\nconst g=f\nprint(g())\n",
    "struct C { value:i64\n constructor(value=this.value) { this.value=value } }\n",
    "struct C { value:i64=2\n f(x:i64=this.value)->i64 { return x } }\n",
    "struct C { const value:string\n constructor(value,n:i64) { var i=0\n while(i<n) { this.value=value\n i=i+1 }\n this.value=value } }\n",
    "struct C { const value:string\n constructor(value,flag:bool) { if(flag) { return }\n this.value=value } }\n",
    "struct C { const value:string\n constructor(value) { this.get()\n this.value=value }\n get()->string{return this.value} }\n",
    "struct C<T> { const value:T\n constructor(value) { this.value=value+value } }\n",
    "struct C { private const value:string\n constructor(value){this.value=value} }\nconst bad=C{value:\"hidden\"}\n",
    "struct C { const value:string\n constructor(value){this.value=value} }\nconst bad=C()\n",

    "struct C { const value:string\n constructor(value) {} }\n",
    "struct C { const value:string\n constructor(value) { this.value=value\n this.value=value } }\n",
    "struct C { const value:string\n constructor(value, ok:bool) { if(ok) { this.value=value } } }\n",
    "struct C { const value:string\n constructor(value) { return\n this.value=value } }\n",
    "struct C { const value:string\n constructor(other) { this.value=other } }\n",
    "struct C { const value:string\n constructor(value) { const capture=fn()->string { return this.value }\n this.value=value } }\n",
    "struct C { const value:string=\"default\"\n constructor(value) { this.value=value } }\n",
    "struct C { const value:string\n private constructor(value) { this.value=value } }\nconst c=C(\"bad\")\n",
    "struct C { const value:string\n constructor(value) { this.value=value } }\nconst c=C(1)\n",
    "struct C { const value:string\n constructor(value) { this.value=value\n return this } }\n",

    "var values=[]\n",
    "const values:Array<i64>=[\"wrong\"]\n",
    "const values=[1,\"wrong\"]\n",
    "const values:Array<()> = []\n",
    "const values=[1]\nvalues.push(2)\n",
    "const values=[1]\nvalues[0]=2\n",
    "fn unused(values:Array<i64>) { values.push(2) }\n",
    "fn unused(values:Array<i64>) { values.set(0,2) }\n",
    "fn unused<T>(values:Array<T>) { values[true] }\n",
    "fn unused<T>(values:Array<T>) { print(values[0]) }\n",
    "fn unused<T>(values:Array<T>) { const invalid:Array<i64> = values }\n",
    "var values=[1]\nvalues.map(fn(value:i64)->i64 { return value })\n",
    "var values=[1]\nvalues.iterator()\n",
    "var values=[1]\nvalues.ptr()\n",
    "var values=[1]\nvalues.capacity\n",
    "var values=[1]\nvalues.push<string>(\"wrong\")\n",
    "var values=[1]\nvalues.get()\n",
    "var values=[1]\nvalues.set(0)\n",
    "var values=[1]\nvalues[0]=\"wrong\"\n",
    "const len=7\nlen([1])\n",
    "fn made()->Array<i64> { return [1] }\nmade().push(2)\n",
    "fn made()->Array<i64> { return [1] }\nmade()[0]=2\n",
    "fn unused(f:fn(i64)->i64)->i64 { return f(true) }\n",
    "fn unused(f:fn(i64)->i64)->i64 { return f() }\n",
    "fn unused(f:fn(i64)->i64)->i64 { return f<i64>(1) }\n",
    "fn unused(f:fn(i64)->i64)->string { return f(1) }\n",
    "fn unused(f:fn(ref i64)->i64) {}\n",
    "fn unused(f:fn(move string)->string) {}\n",
    "fn unused<T>(f:fn(T)->T) { f(1) }\n",
    "fn unused<T>(f:fn(T)->T,x:T)->T { return f(x)+x }\n",
    "fn unused<T>(f:fn(T)->T,x:T)->bool { return f(x) }\n",
    "fn apply<T>(f:fn(T)->T,x:T)->T { return f(x) } fn text(x:string)->string { return x } apply<i64>(text,1)\n",
    "fn need<T:Sendable>(x:T) {} fn unused<T>(f:fn(T)->T) { need<fn(T)->T>(f) }\n",
    "fn f(x:i64)->i64 { return x } const g:fn(bool)->i64=f\n",
    "fn f<T>(x:T)->T { return x } const g=f\n",
    "import \"./lib\" as lib\nconst f=lib.hidden\n",
    "fn send<T:Sendable>(x:T)->T { return x } fn f()->i64 {return 1} const g=send<fn()->i64>(f)\n",

    "Coro.yield(1)\n",
    "Coro.yield<i64>()\n",
    "const x:i64 = Coro.yield()\n",
    "const Coro = Atomic(1)\nCoro.yield()\n",
    "fn unused(Coro:i64) { Coro.yield() }\n",
    "Coro.missing()\n",
    "const alias = Coro\nalias.yield()\n",
    "fn unused()->i64 { return Coro.yield() }\n",
    "fn unused<T>(value:T)->T { return true ? value : Coro.yield() }\n",

    "fn unused() -> i64 { return true }\n",
    "fn unused<T>(value: T) -> T { return value.missing() }\n",
    "fn value(x: i64) -> i64 { return x }\nvalue(true)\n",
    "fn value(x: i64) -> i64 { return x }\nvalue()\n",
    "const a = 1\na = 2\n",
    "const a: string = 1\n",
    "const a = 1\nconst a = 2\n",
    "print(Atomic(1), 2, 3)\n",
    "print(Atomic(1))\n",
    "__writeStderr(\"forbidden\")\n",
    "import { __writeStderr } from \"std/io/output\"\n",
    "import { writeStderr } from \"std/io/output\"\nwriteStderr(1)\n",
    "import \"std/io/output\" as output\noutput.__writeStdout(\"forbidden\")\n",
    "const x:f32=16777217\n",
    "const x:f64=9007199254740993\n",
    "const x:f32=18446744073709551615\n",
    "const x:i64=1.25\n",
    "const x:f32=1e\n",
    "const x=1.0e+\n",
    "const x=1._0\n",
    "const x=1.0_e1\n",
    "const x=1.0e_1\n",
    "const x=1__0.0\n",
    "const x:f32=(1.25 as f64)\n",
    "const x:f32=1 as f64\n",
    "const x=(1 as f32)+ (2 as i32)\n",
    "const x=(1 as f32)% (2 as f32)\n",
    "const x=~(1 as f32)\n",
    "const x=true ? (1 as f32) : 1\n",
    "print(Atomic(1))\n",
    "print(fn()->f32 { return 1.0 })\n",
    "fn unused<T>(v:T)->f32 { return v as f32 }\n",
    "fn unused<T>(v:T)->bool { return v < (1 as f64) }\n",
    "const x=true as f32\n",
    "const x=(1 as f32) as bool\n",
    "const x = true as i8\n",
    "const x = (1 as i8) as bool\n",
    "const x = 1 as u8?\n",
    "fn bad<T>(x:T)->u64 { return x as u64 }\n",
    "fn bad<T>(x:i8)->T { return x as T }\n",
    "const x = (1 as i8) + (1 as u8)\n",
    "const x = (1 as u64) < (1 as i64)\n",
    "fn bad() { var x = 1 as i8; x += (1 as i16) }\n",
    "const x:i8 = 128\n",
    "const x:i8 = -129\n",
    "const x:i16 = 32768\n",
    "const x:i16 = -32769\n",
    "const x:i32 = 2147483648\n",
    "const x:i32 = -2147483649\n",
    "const x = 9223372036854775808\n",
    "const x = -9223372036854775809\n",
    "const x:u8 = 256\n",
    "const x:u16 = 65536\n",
    "const x:u32 = 4294967296\n",
    "const x:u64 = -1\n",
    "const x:u64 = 18446744073709551616\n",
    "const wide:i64 = 1; const x:i8 = wide\n",
    "const signed:i8 = 1; const x:u64 = signed\n",
    "fn unused() { var x:i8 = 1; x = 128 }\n",
    "fn unused(x:i64) { var narrow:i8 = 1; narrow = x }\n",
    "fn unused()->i8 { return 128 }\n",
    "fn unused(x:i64)->i8 { return x }\n",
    "fn unused()->u64 { return -1 }\n",
    "fn f(x:u8) {} f(256)\n",
    "fn f(x:i64) {} const x:u8 = 1; f(x)\n",
    "fn id<T>(x:T)->T { return x } id<u8>(256)\n",
    "fn f(x:i8) {} const g=f; g(128)\n",
    "fn unused<T>()->T { return 1 }\n",
    "fn unused<T>(x:T)->i64 { return x }\n",
    "const narrow:i8=1; const wide:i64 = narrow + 128\n",
    "const narrow:i8=1; const wide:i64 = 128 + narrow\n",
    "const x:u8 = true ? 1 : -1\n",
    "const x:i8 = false ? 128 : 1\n",
    "fn unused() { var x:u8=0; x += -1 }\n",
    "const x = 18446744073709551616 as u64\n",
    "const x = (-9223372036854775809) as u64\n",
    "const x = 18446744073709551615 as bool\n",
    "const x = 18446744073709551615 as u64?\n",
    "const x = (18446744073709551615 + 0) as i8\n",
    "const x = true ? (1 as i8) : (1 as u16)\n",
    "const x = false ? true : (1 as i8)\n",
    "fn unused<T>(x:T) { const y = true ? x : (1 as i8) }\n",
    "const a = Atomic(true)\n",
    "const a = Atomic(1)\na.fetchAdd(false)\n",
    "const a = Atomic(1)\na.load(1)\n",
    "print(missing)\n",
    "print(later)\nconst later = 1\n",
    "fn value() -> i64 {}\n",
    "fn value() { return 1 }\n",
    "fn value(x: i64, x: i64) -> i64 { return x }\n",
    "fn value(x: string) { x = \"modified\" }\n",
    "fn value() { var x = 1; x = true }\n",
    "fn value() { var x = 1; var x = 2 }\n",
    "import \"./lib\" as lib\nlib.hidden()\n",
    "import { hidden } from \"./lib\"\n",
    "import { missing } from \"./lib\"\n",
    "import \"./lib\" as lib\nconst lib = 1\n",
    "import \"./missing\" as missing\n",
    "import \"./lib.xr\" as lib\n",
    "const print = 1\nprint(1)\n",
    "const Atomic = 1\nAtomic(1)\n",
    "fn unused() { while true {} }\n",
    "fn unused() { if (1) {} }\n",
    "fn unused() { while (1) {} }\n",
    "fn unused(x: bool) -> i64 { if (x) { return 1 } }\n",
    "fn unused(x: bool) { if (x) { var hidden = 1 } print(hidden) }\n",
    "fn unused() { break }\n",
    "fn unused() { continue }\n",
    "fn unused() { while (true) { break; print(1) } }\n",
    "fn unused(x: bool) { if (x) { return } else { return } print(1) }\n",
    "if (true) { return }\n",
    "fn unused<T>(x:T) { if (true) { print(x) } }\n",
    "fn unused<T>(x:T) { var y=x; while (y < x) {} }\n",
    "fn unused<T>(x:T)->T { return ~x }\n",
    "fn unused<T>(x:T)->T { return x << 1 }\n",
    "fn unused<T>(x:T)->T { return x & x }\n",
    "fn unused()->i64 { return true & false }\n",
    "fn unused()->i64 { return 1 << false }\n",
    "fn unused()->i64 { return ~true }\n",
    "fn unused()->i64 { return \"x\" | \"y\" }\n",
    "fn unused() { const x=1; x <<= 1 }\n",
    "fn unused() { var x=true; x ^= false }\n",
    "fn unused<T>(x:T) { var y=x; y += x }\n",
    "fn unused(x:i64) { x &= 1 }\n",
    "fn unused() { var x=\"a\"; x -= \"b\" }\n",
    "fn unused() { var x=\"a\"; x += 1 }\n",
    "fn unused() { var x=1; x += true }\n",
    "fn unused() -> i64 { return 1 + true }\n",
    "fn unused() { print(!1) }\n",
    "fn unused() { print(false && 1) }\n",
    "fn unused() { print(true || missing) }\n",
    "fn unused<T>(x:T) { print(false && x) }\n",
    "fn unused() { for (; 1;) { break } }\n",
    "fn unused() { for (var i=0; i<1; i++) {} print(i) }\n",
    "fn unused() { for (var i=0; true; i=hidden) { var hidden=1; break } }\n",
    "fn unused() { for (;; missing()) { break } }\n",
    "fn unused<T>(x:T) { for (;; x+1) { return } }\n",
    "fn unused() { for (;;) { break; print(1) } }\n",
    "fn unused() { const i=1; i++ }\n",
    "fn unused() { var i=true; i++ }\n",
    "fn unused(i:i64) { i-- }\n",
    "fn unused() { var i=1; print(i++) }\n",
    "fn unused() { for (var i=1;; i++) { const i=true; i++ } }\n",
    "fn unused<T>(x:T)->T { return x * x }\n" ,
    "fn unused<T>(x:T)->T { return -x }\n" ,
    "fn unused<T>(x:T)->bool { return x >= x }\n" ,
    "fn unused()->i64 { return true - 1 }\n" ,
    "fn unused()->i64 { return 1 / false }\n" ,
    "fn unused()->i64 { return -true }\n" ,
    "fn unused()->i64 { return \"x\" % 2 }\n" ,
    "fn unused()->bool { return \"x\" <= \"y\" }\n" ,
    "const a = \"unterminated\n"
};
#include "xir_source_integer_context.h"
#include "xir_source_decimal_context.h"
static void static_import_authority(const XrXirSourceRequest *request, const char *root, const char *library) {
    write_source(library,
        "export struct Factory<T> { const value:T\n private constructor(value){this.value=value}\n"
        " static make(value:T)->Factory<T>{return Factory<T>(value)}\n"
        " private static secret()->i64{return 7}\n }\n"
        "struct Hidden {static value()->i64{return 1}}\n");
    const char *const accepted[] = {
        "import {Factory} from \"./lib\"\nconst f=Factory<i64>.make\nconst c=f(7)\nprint(c.value)\n",
        "import \"./lib\" as lib\nconst c=lib.Factory<string>.make(\"ok\")\nprint(c.value)\n"
    };
    const char *const invalid[] = {
        "import \"./lib\" as lib\nlib.Factory<i64>.secret()\n",
        "import \"./lib\" as lib\nconst f=lib.Factory<i64>.secret\n",
        "import {Factory} from \"./lib\"\nconst c=Factory<i64>(7)\n",
        "import \"./lib\" as lib\nlib.Hidden.value()\n",
        "import {Factory} from \"./lib\"\nfn bad(){const Factory=7\nFactory<i64>.make(1)}\n"
    };
    for (unsigned group = 0; group < 2; ++group) {
        const char *const *sources = group ? invalid : accepted;
        size_t count = group ? sizeof(invalid)/sizeof(invalid[0]) : sizeof(accepted)/sizeof(accepted[0]);
        for (size_t i = 0; i < count; ++i) {
            write_source(root, sources[i]);
            XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
            XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
            if (!group && status != XR_XIR_OK) fprintf(stderr, "static import %zu: %s\n", i, diagnostic.message);
            CHECK(group ? status != XR_XIR_OK && !result.checked : status == XR_XIR_OK && result.checked);
            xr_xir_source_result_free(&result);
        }
    }
}
static void constructor_admission(const XrXirSourceRequest *request, const char *root) {
    const char *const sources[] = {
        "struct C<T>{static f<U>(value:U)->U{return value}}\nC<i64>.f(1)\n",
        "fn require<T:Sendable>(value:T)->T{return value}\nstruct C<T:Sendable>{static f<U:Sendable>(outer:T,value:U)->U{return require<U>(value)}}\nprint(C<i64>.f<string>(7,\"yes\"))\n",

        "struct C { static value<T>(x:T)->T { return x } }\nprint(C.value<i64>(7))\nconst f=C.value<string>\nprint(f(\"yes\"))\n",
        "struct C<T> { value:T\n map<U>(x:U, f:fn(T,U)->U)->U { return f(this.value,x) }\n static choose<U>(x:T,y:U)->U { return y } }\nconst c=C<i64>{value:7}\nprint(C<i64>.choose<string>(7,\"yes\"))\nconst f=c.map<string>\nprint(f(\"ok\",fn(a:i64,b:string)->string {return b}))\n",

        "struct C { static value(x:i64=7)->i64 { return x } }\nprint(C.value(),C.value(9))\nconst f=C.value\nprint(f(8))\n",
        "struct C<T> { static value(x:T)->T { return x } }\nprint(C<i64>.value(7))\nconst f=C<string>.value\nprint(f(\"yes\"))\n",
        "struct C { const value:string\n private constructor(value) { this.value=value }\n private static secret()->string { return \"private\" }\n static make(value:string=C.secret())->C { return C(value) }\n static capture()->fn()->string { return C.secret } }\nprint(C.make().value)\nconst f=C.capture()\nprint(f())\n",
        "struct C<T> { static value(x:T, f:fn(T)->T=fn(y:T)->T { return y })->T { return f(x) } }\nprint(C<i64>.value(7))\n",
        "struct C { static value()->i64 { return 7 } }\nfn shadow()->i64 { const C=fn()->i64 { return 9 }\nreturn C() }\nprint(shadow())\n",

        "struct C { const value:i64\n private constructor(value:i64=7) { this.value=value }\n get(other:C=C())->i64 { return other.value } }\n",
        "fn value(x:string=\"ok\")->string { return x }\nprint(value())\nprint(value(\"explicit\"))\n",
        "fn make<T>(x:fn()->T)->T { return x() }\nfn value<T>(x:fn()->T=fn()->T { var items:Array<T> = []\n return items[0] })->T { return x() }\n",
        "struct C { const value:string\n constructor(value=\"ok\") { this.value=value }\n read(s:string=\"method\")->string { return s+this.value } }\nconst c=C()\nprint(c.read())\n",
        "fn privateValue()->string { return \"private\" }\nexport fn value(x:string=privateValue())->string { return x }\nprint(value())\n",
        "fn nested(x:i64=3)->i64 { return x }\nfn outer(x:i64=nested())->i64 { return x }\nprint(outer())\n",
        "const a:i64=9\nfn f(a:i64,b:i64=a)->i64 { return b }\nprint(f(1))\n",
        "struct C { const factory:fn()->string=fn()->string { return \"ready\" }\n const value:string\n constructor() { this.value=this.factory() } }\nconst c=C()\n",
        "struct C { const value:string\n constructor(value) { this.value=value } }\nconst c=C(\"yes\")\nprint(c.value)\n",
        "struct C<T> { const value:T\n constructor(value) { this.value=value } }\nconst c=C<string>(\"yes\")\nprint(c.value)\n",
        "struct C { const value:string\n constructor(value,ok:bool) { if(ok) { this.value=value } else { this.value=\"no\" } } }\nconst c=C(\"yes\",true)\n",
        "struct C { const value:string=\"default\"\n constructor() {} }\nconst c=C()\n",
        "struct C { const first:string\n const second:string\n constructor(first) { this.first=first\n this.second=this.first } }\nconst c=C(\"yes\")\n",
        "struct C { const value:string\n constructor(value) { this.value=value\n const capture=fn()->string { return this.value }\n print(capture())\n return } }\nconst c=C(\"yes\")\n",
    };
    for (unsigned i = 0; i < sizeof(sources)/sizeof(sources[0]); ++i) {
        write_source(root, sources[i]);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
        if (status != XR_XIR_OK) fprintf(stderr, "constructor case %u: %s\n", i, diagnostic.message);
        CHECK(status == XR_XIR_OK && result.checked);
        xr_xir_source_result_free(&result);
    }
}
static void enum_admission(const XrXirSourceRequest *request, const char *root) {
    const char *sources[] = {
        "enum E{Bad}\nfn erase<T:Error>(value:T)->Error{return value}\nfn raise(value:Error){throw value}\nraise(erase<E>(E.Bad))\n",
        "enum E{Bad}\nstruct Box{value:Error}\nfn raise(value:Box){throw value.value}\nraise(Box{value:E.Bad})\n",
        "fn first(values:Array<Error>)->Error{return values[0]}\n",
        "fn relay(value:Error)->Error{return value}\n",
        "enum E{Bad}\nfn raise<T:Error>(value:T)->i64{throw value}\nraise<E>(E.Bad)\n",
        "fn forward<E:Error & Sendable>(value:E)->i64{throw value}\n",
        "fn raise<T:Error>(value:T)->i64{throw value}\nfn forward<U:Error>(value:U)->i64{return raise<U>(value)}\n",
        "enum E{Bad}\nstruct Box<T:Error>{value:T\nraise()->i64{throw this.value}}\nBox<E>{value:E.Bad}.raise()\n",
        "enum E{Bad}\nstruct Box{static raise<T:Error>(value:T)->i64{throw value}}\nBox.raise<E>(E.Bad)\n",
        "enum E{Bad}\nthrow E.Bad\n",
        "enum E{Bad{message:string}}\nfn unused()->i64{throw E.Bad{message:\"owned\"}}\n",
        "enum E<T:Sendable>{Bad{value:T}}\nfn unused<T:Sendable>(value:T)->i64{throw E<T>.Bad{value:value}}\n",
        "enum E{Bad{message:string}}\nfn make(value:string)->fn()->i64{return fn()->i64{throw E.Bad{message:value}}}\n",
        "enum E{Bad}\nstruct S{value:string\ninit(){throw E.Bad}}\n",
        "fn f(x:i64)->i64{return match(x){1,2->3}}\n",
        "fn f(x:bool)->i64{return match(x){true->3}}\n",
        "fn f(x:bool)->bool{return match(x){v->v}}\n",
        "fn f(x:f32)->i64{return match(x){_,16777216->1}}\n",
        "fn f(x:f32)->i64{return match(x){1.0->3}}\n",
        "fn f(x:string)->string{return match(x){\"x\"->\"yes\"}}\n",
        "fn f(x:i64)->i64{match(x){1->{return 3}}}\n",
        "enum E{A{x:i8}}\nfn f(v:E)->i64{return match(v){E.A{x:-128..0}->1,E.A{x:0..=127}->2}}\n",
        "enum E{A{x:u64}}\nfn f(v:E)->i64{return match(v){E.A{x:0..18446744073709551615}->1,E.A{x:18446744073709551615}->2}}\n",
        "enum E{A{x:i8,b:bool}}\nfn f(v:E)->i64{return match(v){E.A{x:-128..0,b:true}->1,E.A{x:-128..0,b:false}->2,E.A{x:0..=127}->3}}\n",
        "enum E{A{x:f32}}\nfn f(v:E)->i64{return match(v){E.A{x:16777216}->1,E.A{}->2}}\n",

        "enum E{A{x:i8,y:u64,s:string,f:f32},B}\nfn f(v:E)->i64{return match(v){E.A{x:-128}->1,E.A{y:18446744073709551615}->2,E.A{s:\"yes\",f:-0.0}->3,E.A{}->4,E.B->5}}\n",

        "enum I{A,B{value:string}}\nenum O{None,Some{inner:I,flag:bool}}\nfn pick(v:O)->string{return match(v){O.None->\"none\",O.Some{inner:I.A,flag:true}->\"at\",O.Some{inner:I.A,flag:false}->\"af\",O.Some{inner:I.B{value},flag:true}->value,O.Some{inner:I.B{value},flag:false}->\"bf\"}}\nprint(pick(O.Some{inner:I.B{value:\"yes\"},flag:true}))\n",
        "enum I<T>{None,Some{value:T}}\nenum O<T>{Wrap{inner:I<T>}}\nfn pick<T>(v:O<T>,fallback:T)->T{return match(v){O.Wrap{inner:I.None}->fallback,O.Wrap{inner:I.Some{value}}->value}}\nprint(pick<i64>(O<i64>.Wrap{inner:I<i64>.Some{value:42}},0))\n",
        "enum E{A{x:bool,y:bool}}\nconst n=match(E.A{x:true,y:false}){E.A{x:true,y:true}->1,E.A{x:true,y:false}->2,E.A{x:false,y:true}->3,E.A{x:false,y:false}->4}\nprint(n)\n",

        "enum E{A,B}\nfn f(v:E)->i64{match(v){E.A->{return 1},E.B->{return 2}}}\nprint(f(E.B))\n",
        "enum E{A,B}\nfn f(v:E)->i64{const x=match(v){E.A->{return 1},E.B->2}\nreturn x}\nprint(f(E.B))\n",
        "enum E{A,B}\nfn f(v:E)->i64{var n=0\nwhile(n<3){n=n+1\nmatch(v){E.A->{continue},E.B->{break}}}\nreturn n}\nprint(f(E.A))\n",
        "enum E { A, B }\nconst x=match(E.B){E.A->1,E.B->2}\nprint(x)\n",
        "enum E<T> { Empty, Some { value:T } }\nfn pick<T>(v:E<T>,fallback:T)->T{return match(v){E.Some{value}->value,E.Empty->fallback}}\nprint(pick<i64>(E<i64>.Some{value:42},0))\n",
        "enum E { A { x:string }, B }\nconst v=E.A{x:\"yes\"}\nconst x=match(v){E.A{x} if(false)->x,E.A{x}-> {Coro.yield()\nx},E.B->\"empty\"}\nprint(x)\n",
        "enum E { A { x:i64 }, B }\nfn f(v:E)->i64{const x=\"outer\"\nconst read=fn()->i64{return match(v){E.A{x}->x,E.B->0}}\nreturn read()}\nprint(f(E.A{x:42}))\n",
        "enum E{A{x:string},B{x:string}}\nfn f(v:E)->string{return match(v){E.A{x},E.B{x}->x}}\nprint(f(E.B{x:\"b\"}))\n",
        "enum E{A{x:i64,y:string},B{x:i64,y:string}}\nfn f(v:E)->string{return match(v){E.A{x,y},E.B{y,x}->y}}\nprint(f(E.B{x:1,y:\"b\"}))\n",
        "enum E<T>{A{x:T},B{x:T}}\nfn f<T>(v:E<T>)->T{return match(v){E.A{x},E.B{x}->x}}\nprint(f<string>(E<string>.B{x:\"b\"}))\n",
        "enum E{A,B}\nfn f(v:E)->i64{return match(v){E.A,E.B->1}}\nprint(f(E.A))\n",
        "enum E{A,B}\nfn f(v:E)->i64{return match(v){_,_->1}}\nprint(f(E.B))\n",
        "enum E{A,B}\nfn f(v:E)->i64{return match(v){x,x->x.ordinal}}\nprint(f(E.B))\n",
        "enum E{A{x:i64},B{x:i64}}\nfn f(v:E)->i64{const x=\"outer\"\nconst read=fn()->i64{return match(v){E.A{x},E.B{x}->x}}\nreturn read()}\nprint(f(E.B{x:42}))\n",
        "enum Color { Red, Blue }\nprint(Color.Red.ordinal, Color.Blue.ordinal)\n",
        "enum Choice { Empty, Some { value:i64, label:string } }\nconst v=Choice.Some { label:\"yes\", value:42 }\nprint(v.ordinal)\n",
        "enum Choice<T> { Empty, Some { value:T } }\nconst v=Choice<i64>.Some { value:42 }\nprint(v.ordinal)\n",
        "enum Choice<T:Sendable> { Empty, Some { value:T } }\nfn make<T:Sendable>(v:T)->Choice<T> { return Choice<T>.Some { value:v } }\nprint(make<i64>(42).ordinal)\n",
    };
    for (unsigned i=0;i<sizeof(sources)/sizeof(sources[0]);++i) {
        write_source(root,sources[i]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(request,&result,&diagnostic);
        if(status!=XR_XIR_OK) fprintf(stderr,"enum case %u: %s\n",i,diagnostic.message);
        CHECK(status==XR_XIR_OK && result.checked);
        XrXirArtifact *closed=NULL,*lowered=NULL;
        CHECK(xr_xir_specialize(result.checked,NULL,&closed,NULL)==XR_XIR_OK);
        XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
        CHECK(xr_xir_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);
        xr_xir_artifact_free(lowered); xr_xir_artifact_free(closed);
        xr_xir_source_result_free(&result);
    }
}
static void nested_pattern_budgets(const XrXirSourceRequest *request, const char *root) {
    write_source(root,"enum I{A,B{value:string}}\nenum O{Wrap{inner:I,flag:bool}}\n"
        "fn f(v:O)->string{return match(v){O.Wrap{inner:I.A}->\"empty\","
        "O.Wrap{inner:I.B{value},flag:true}->value,O.Wrap{inner:I.B{value},flag:false}->\"false\"}}\n");
    for (unsigned kind=0;kind<2;++kind) {
        XrXirBudget budget=xr_xir_default_budget();
        uint64_t low=0,high=kind?budget.work:budget.metadata_bytes;
        XrXirSourceRequest bounded=*request; bounded.budget=&budget;
        while (low+1<high) {
            uint64_t middle=low+(high-low)/2;
            if (kind) budget.work=middle; else budget.metadata_bytes=middle;
            XrXirSourceResult result={0};
            XrXirStatus status=xr_xir_source_check(&bounded,&result,NULL);
            CHECK(status==XR_XIR_OK || status==XR_XIR_BUDGET);
            if (status==XR_XIR_OK) {CHECK(result.checked && result.snapshot);high=middle;}
            else {CHECK(!result.checked && !result.snapshot);low=middle;}
            xr_xir_source_result_free(&result);
        }
        for (unsigned exact=0;exact<2;++exact) {
            if (kind) budget.work=exact?high:high-1; else budget.metadata_bytes=exact?high:high-1;
            XrXirSourceResult result={0};
            XrXirStatus status=xr_xir_source_check(&bounded,&result,NULL);
            CHECK(status==(exact?XR_XIR_OK:XR_XIR_BUDGET));
            CHECK(exact?(result.checked && result.snapshot):(!result.checked && !result.snapshot));
            xr_xir_source_result_free(&result);
        }
    }
}
static void unit_slot_admission(const XrXirSourceRequest *request, const char *root) {
    const char *sources[] = {"const x = Coro.yield()\n",
        "const x:() = Coro.yield()\n", "var x:()\n"};
    for (uint32_t run=0;run<3;++run) {
        write_source(root,sources[run]);
        XrXirSourceResult result={0}; XrXirSourceDiagnostic diagnostic={0};
        CHECK(xr_xir_source_check(request,&result,&diagnostic)==XR_XIR_OK);
        CHECK(result.checked && result.snapshot && diagnostic.status==XR_XIR_OK);
        CHECK(xr_xir_artifact_verify(result.checked,NULL,NULL)==XR_XIR_OK);
        const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
        CHECK(view && view->complete);
        uint32_t bindings=0;
        for (uint32_t d=0;d<view->declaration_count;++d) {
            const XrXirSourceDeclaration *binding=&view->declarations[d];
            if (binding->kind!=XR_XIR_SOURCE_BINDING || strcmp(binding->name,"x")) continue;
            CHECK(binding->type.known && binding->type.type==XR_XIR_UNIT &&
                binding->mutable==(run==2));
            ++bindings;
        }
        CHECK(bindings==1);
        const XrXirModule *module=xr_xir_artifact_module(result.checked);
        const XrXirDeclarations *declarations=module->declarations;
        CHECK(declarations && declarations->slot_count==1 &&
            declarations->slots[0].type==XR_XIR_UNIT && declarations->slots[0].mutable==(run==2 ? 1u : 0u));
        uint32_t initializer=declarations->modules[declarations->root_module].initializer;
        CHECK(initializer<module->function_count);
        const XrXirFunction *function=&module->functions[initializer];
        uint32_t suspends=0,publishes=0;
        for (uint32_t i=0;i<function->instruction_count;++i) {
            const XrXirInstruction *op=&function->instructions[i];
            if (op->op==XR_XIR_SUSPEND) { CHECK(!publishes); ++suspends; }
            if (op->op==XR_XIR_SLOT_INIT) {
                CHECK(op->immediate==0 && !op->args[0] && !op->args[1]); ++publishes;
            }
        }
        CHECK(publishes==1 && suspends==(run<2 ? 1u : 0u));
        xr_xir_source_result_free(&result);
    }
}
int main(void) {
    stdlib_resolution();

    char directory[XR_TEST_PATH_MAX] = "xir-source-admission-XXXXXX";
    CHECK(xr_test_mkdtemp(directory));
    char absolute[XR_TEST_PATH_MAX];
    CHECK(xr_test_realpath_buf(directory, absolute, sizeof(absolute)));
    char root[XR_TEST_PATH_MAX], library[XR_TEST_PATH_MAX];
    CHECK(snprintf(root, sizeof(root), "%s/root.xr", absolute) > 0);
    CHECK(snprintf(library, sizeof(library), "%s/lib.xr", absolute) > 0);
    write_source(library, "fn hidden() -> i64 { return 1 }\nexport fn visible() -> i64 { return 2 }\n");
    XrCompilerSession *session = xr_compiler_session_new(NULL); CHECK(session);
    primitive_authority(session, absolute);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, absolute};
    XrXirSourceRequest request = {session, root, &authority, NULL, XR_SOURCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    unit_slot_admission(&request,root);
    source_integer_contexts(&request, root);
    source_decimal_contexts(&request, root);
    enum_admission(&request, root);
    nested_pattern_budgets(&request, root);
    write_source(root,"const x:i8 = (1 + 2) + 3\n");
    XrXirSourceResult recursive_literal = {0};
    CHECK(xr_xir_source_check(&request,&recursive_literal,NULL) == XR_XIR_OK);
    CHECK(recursive_literal.checked != NULL);
    const XrXirModule *literal_module = xr_xir_artifact_module(recursive_literal.checked);
    CHECK(literal_module && literal_module->declarations->slot_count == 1);
    CHECK(literal_module->declarations->slots[0].type == XR_XIR_I8);
    xr_xir_source_result_free(&recursive_literal);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        write_source(root, rejected[i]);
        XrXirArtifact *artifact = NULL; XrXirSourceDiagnostic diagnostic;
        XrXirSourceResult query_result_9 = {0};
        XrXirStatus query_status_9 = xr_xir_source_check(&request, &query_result_9, &diagnostic);
        artifact = query_result_9.checked; query_result_9.checked = NULL;
        xr_xir_source_result_free(&query_result_9);
        XrXirStatus status = query_status_9;
        if (status == XR_XIR_OK) fprintf(stderr, "incorrectly admitted source case %zu\n", i);
        CHECK(status != XR_XIR_OK && !artifact && diagnostic.status == status && diagnostic.message[0]);
    }
    write_source(root,"fn unused<T>(f:fn(T)->T) {}\n");
    XrXirArtifact *unused = NULL, *closed_unused = NULL;
    XrXirSourceResult query_result_10 = {0};
    XrXirStatus query_status_10 = xr_xir_source_check(&request, &query_result_10, NULL);
    unused = query_result_10.checked; query_result_10.checked = NULL;
    xr_xir_source_result_free(&query_result_10);
    CHECK(query_status_10 == XR_XIR_OK && unused);
    CHECK(xr_xir_specialize(unused,NULL,&closed_unused,NULL) == XR_XIR_OK && closed_unused);
    CHECK(!xr_xir_artifact_module(closed_unused)->types);
    xr_xir_artifact_free(closed_unused); xr_xir_artifact_free(unused);
    const char *valid = "import { visible } from \"./lib\"\nprint(visible(), true)\n";
    write_source(root, valid);
    for (unsigned mode = 0; mode < 5; ++mode) {
        XrXirBudget budget = xr_xir_default_budget();
        if (mode == 0) budget.metadata_bytes = 1;
        if (mode == 1) budget.work = 0;
        if (mode == 2) budget.instructions = 1;
        if (mode == 3) budget.functions = 1;
        if (mode == 4) budget.blocks = 0;
        request.budget = &budget;
        XrXirArtifact *artifact = NULL;
        XrXirSourceResult query_result_11 = {0};
        XrXirStatus query_status_11 = xr_xir_source_check(&request, &query_result_11, NULL);
        artifact = query_result_11.checked; query_result_11.checked = NULL;
        xr_xir_source_result_free(&query_result_11);
        CHECK(query_status_11 == XR_XIR_BUDGET && !artifact);
    }
    request.budget = NULL;
    write_source(library, "var forbidden = 1\nexport fn visible() -> i64 { return forbidden }\n");
    XrXirArtifact *artifact = NULL;
    XrXirSourceResult query_result_12 = {0};
    XrXirStatus query_status_12 = xr_xir_source_check(&request, &query_result_12, NULL);
    artifact = query_result_12.checked; query_result_12.checked = NULL;
    xr_xir_source_result_free(&query_result_12);
    CHECK(query_status_12 != XR_XIR_OK && !artifact);
    write_source(library, "import \"./root\" as root\nexport fn visible() -> i64 { return 1 }\n");
    XrXirSourceResult query_result_13 = {0};
    XrXirStatus query_status_13 = xr_xir_source_check(&request, &query_result_13, NULL);
    artifact = query_result_13.checked; query_result_13.checked = NULL;
    xr_xir_source_result_free(&query_result_13);
    CHECK(query_status_13 != XR_XIR_OK && !artifact);
    write_source(library, "export fn visible() -> i64 { return 2 }\n");
    XrXirSourceResult query_result_14 = {0};
    XrXirStatus query_status_14 = xr_xir_source_check(&request, &query_result_14, NULL);
    artifact = query_result_14.checked; query_result_14.checked = NULL;
    xr_xir_source_result_free(&query_result_14);
    CHECK(query_status_14 == XR_XIR_OK && artifact);
    xr_xir_artifact_free(artifact);
    static_import_authority(&request, root, library);
    write_source(library, "export fn visible() -> i64 { return 2 }\n");
    constructor_admission(&request, root);
    shadowed_coro(&request, root);
    source_catch_cases(&request, root, library);
    source_panic_cases(&request, root);
    source_cleanup_admission(&request, root);
    source_conditional_cases(&request, root);
    xr_compiler_session_delete(session);
    CHECK(xr_test_unlink(root) == 0 && xr_test_unlink(library) == 0 && xr_test_rmdir(directory) == 0);
    puts("Source declaration, visibility, type, graph and budget rejection passed");
    return 0;
}
