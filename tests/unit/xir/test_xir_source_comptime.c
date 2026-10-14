/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_comptime.c - Genuine two-construction Source constants
 *
 * KEY CONCEPT:
 *   Caller death precedes complete packet receiving and real VM execution.
 *   Allocation injection observes the sole finite compiler ledger implementation.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_ctfe.h"
static const XrCompileResourceLimits source_comptime_test_limits={
    UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char source_comptime_test_text[]=
    "fn leaf(x:i64)->i64 { return x+1 }\n"
    "fn middle(x:i64)->i64 { return leaf(x)+1 }\n"
    "export fn run()->i64 { return comptime middle(40) }\n";
static const char source_comptime_local_text[]=
    "fn positive(x:i64)->bool { return x>0 }\n"
    "fn admitted(x:i64)->bool { return positive(x) && (x==6 || x==7) }\n"
    "fn answer()->i64 { return admitted(6) ? 42 : 0 }\n"
    "export fn run()->i64 { return comptime answer() }\n";
static const char source_comptime_unit_text[]=
    "fn noop() {}\n"
    "fn answer()->i64 { noop(); return 42 }\n"
    "export fn run()->i64 { comptime {}; comptime { const x=6; const y=7 }; comptime noop(); return comptime answer() }\n";
static const char source_comptime_rune_text[]=
    "fn same(a:rune,b:rune)->bool { return a==b }\n"
    "fn answer()->i64 { const c=rune(128512); return same(c,rune(128512)) && i64(c)==128512 ? 42 : 0 }\n"
    "export fn run()->i64 { comptime 'A'; return comptime answer() }\n";
static const char source_comptime_cell_text[]=
    "fn answer()->i64 { var i=0; var total=0; while (i<6) { total=total+7; i=i+1 }; return total }\n"
    "export fn run()->i64 { comptime { var u:()=(); u=() }; return comptime answer() }\n";
static const char source_comptime_string_text[]=
    "fn join(a:string,b:string)->string { return a+b }\n"
    "fn answer()->i64 { return join(\"ab\",\"cd\")==\"abcd\" ? 42 : 0 }\n"
    "export fn run()->i64 { comptime join(\"ab\",\"cd\"); return comptime answer() }\n";
static const char source_comptime_string_read_text[]=
    "fn answer()->i64 { const t=\"abcabc\"; return t.contains(\"bc\") && t.startsWith(\"ab\") && t.endsWith(\"bc\") && \"a\"<\"b\" && t.indexOf(\"bc\",2)==4 && t.lastIndexOf(\"ab\")==3 ? 42 : 0 }\n"
    "export fn run()->i64 { return comptime answer() }\n";
static const char source_comptime_tuple_text[]=
    "fn make()->(i8,string,()) { var p:(i8,string,())=(1,\"a\",()); p=(7,\"ab\",()); return p }\n"
    "fn answer()->i64 { const t=make(); return t.0==7 && t.1==\"ab\" ? 42 : 0 }\n"
    "export fn run()->i64 { comptime ((),()); comptime make(); return comptime answer() }\n";
static const char source_comptime_float_text[]=
    "fn compute(x:f32)->f32 { return x/2.0 }\n"
    "fn answer()->i64 { var x:f32=compute(3.5); return x==1.75 && (0.0/0.0)!=(0.0/0.0) ? 42 : 0 }\n"
    "export fn run()->i64 { comptime compute(3.5); return comptime answer() }\n";
static const char source_comptime_nullable_text[]=
    "fn wrap(x:i8)->i8? { return x }\n"
    "fn make()->(i8,string)? { var x:(i8,string)?=null; x=(7,\"ab\"); return x }\n"
    "fn same(a:string?,b:string?)->bool { return a==b }\n"
    "fn answer()->i64 { const a:string?=\"ab\"; return (wrap(7)??0)==7 && make()!=null && same(a,a) ? 42 : 0 }\n"
    "export fn run()->i64 { comptime make(); return comptime answer() }\n";
static const char *source_comptime_resource_text=source_comptime_test_text;
static XrXirStatus source_comptime_test_resource_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrCompileResourceStats source_comptime_test_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void source_comptime_test_monotonic(XrCompileResourceStats before,XrCompileResourceStats after) {
    CHECK(after.allocation_count>=before.allocation_count && after.allocated_bytes>=before.allocated_bytes);
    CHECK(after.work>=before.work && after.peak_bytes>=before.peak_bytes);
}
/* This is a real public producer call. Caller text, authority and session die
 * before the returned Checked artifact or owned query is consumed. */
static XrXirStatus source_comptime_test_build(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    size_t length=strlen(text);CHECK(length<4096);
    char bytes[4096];memcpy(bytes,text,length+1);char name[]="source-comptime";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));
    authority=(XrModuleIdentityAuthority){0};request=(XrXirSourceRequest){0};input=(XrXirSourceText){0};
    return status;
}
static uint32_t source_comptime_test_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if (body->name_length==length && !memcmp(body->name,name,length)) { CHECK(found==UINT32_MAX);found=f; }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void source_comptime_test_fold(const XrXirModule *module,int64_t expected,XrXirType type) {
    const XrXirFunction *body=&module->functions[source_comptime_test_function(module,"run")];bool present=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        if (op->op==(type==XR_XIR_BOOL?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT) && op->type==type && op->immediate==expected)
            present=true;
    }
    CHECK(present);
}
#include "xir_source_comptime_cases.h"
#include "xir_source_comptime_resources.h"
int main(int argc,char **argv) {
    if (argc==2 && (!strcmp(argv[1],"--resources") || !strcmp(argv[1],"--local-resources") || !strcmp(argv[1],"--unit-resources") || !strcmp(argv[1],"--rune-resources") || !strcmp(argv[1],"--cell-resources") || !strcmp(argv[1],"--string-resources") || !strcmp(argv[1],"--string-read-resources") || !strcmp(argv[1],"--tuple-resources") || !strcmp(argv[1],"--float-resources") || !strcmp(argv[1],"--nullable-resources"))) {
        if (!strcmp(argv[1],"--local-resources")) {
            source_comptime_resource_text=source_comptime_local_text;
            fprintf(stderr,"Source comptime scalar locals whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--unit-resources")) {
            source_comptime_resource_text=source_comptime_unit_text;
            fprintf(stderr,"Source comptime Unit/ordinary scalar result whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--rune-resources")) {
            source_comptime_resource_text=source_comptime_rune_text;
            fprintf(stderr,"Source comptime Rune literal/conversion/equality whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--cell-resources")) {
            source_comptime_resource_text=source_comptime_cell_text;
            fprintf(stderr,"Source comptime private Cell/Unit assignment/loop whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--string-resources")) {
            source_comptime_resource_text=source_comptime_string_text;
            fprintf(stderr,"Source comptime owned String export/parameters/independent replay whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--string-read-resources")) {
            source_comptime_resource_text=source_comptime_string_read_text;
            fprintf(stderr,"Source comptime String comparison/search whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--tuple-resources")) {
            source_comptime_resource_text=source_comptime_tuple_text;
            fprintf(stderr,"Source comptime Tuple/Unit/nested field/Cell independent recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--float-resources")) {
            source_comptime_resource_text=source_comptime_float_text;
            fprintf(stderr,"Source comptime binary Float/Cell/NaN/independent CONST_FLOAT whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--nullable-resources")) {
            source_comptime_resource_text=source_comptime_nullable_text;
            fprintf(stderr,"Source comptime Nullable/Cell/Tuple/unwrap/independent active recipe whole-producer resources\n");
        }
        source_comptime_test_resources();return 0;
    }
    CHECK(argc==1);source_comptime_test_semantics();source_comptime_test_rejections();
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Source comptime genuine scalar/direct closure, two constructions and producer death passed\n");return 0;
}
