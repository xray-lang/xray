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
    if (argc==2 && (!strcmp(argv[1],"--resources") || !strcmp(argv[1],"--local-resources"))) {
        if (!strcmp(argv[1],"--local-resources")) {
            source_comptime_resource_text=source_comptime_local_text;
            fprintf(stderr,"Source comptime scalar locals whole-producer resources\n");
        }
        source_comptime_test_resources();return 0;
    }
    CHECK(argc==1);source_comptime_test_semantics();source_comptime_test_rejections();
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Source comptime genuine scalar/direct closure, two constructions and producer death passed\n");return 0;
}
