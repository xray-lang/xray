/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_diagnostic_resources.c - Actual parser failure ownership and cuts
 */
#include "xray_source_ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "execution/xr_xir_host_execution.c"
#include "api/xsource_api.c"
typedef struct DiagnosticOwners {
    XrSourceCompiler *compiler;
    XrSourceRepl *repl;
    XrSourceAst *ast;
    XrSourceProgram *program;
    XrSourceDiagnostic *diagnostic;
} DiagnosticOwners;
static const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
static XrSourceWriteStatus no_output(void *context,XrSourceOutputStream stream,const char *bytes,size_t length) {
    (void)context;(void)stream;(void)bytes;(void)length;
    CHECK(false);return XR_SOURCE_WRITE_IO;
}
static XrSourceStatus prepare(DiagnosticOwners *owner,unsigned mode,const XrCompileResourceLimits *limits) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status;
    if(mode>=4) {
        XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
        runtime.write=no_output;
        status=xray_source_repl_new(&options,"diagnostic-resources",&runtime,&owner->repl);
    } else status=xray_source_compiler_new(&options,&owner->compiler);
    if(status==XR_SOURCE_OK && mode==3) {
        const char bytes[]="import \"./坏 库\" as Bad\nprint(42)\n";
        const XrSourceText input={XR_DIAGNOSTIC_FIXTURE "/entry.xr",NULL,bytes,sizeof(bytes)-1};
        status=xray_source_parse_text(owner->compiler,&input,&owner->ast,NULL);
    }
    return status;
}
static XrSourceStatus fail_source(DiagnosticOwners *owner,unsigned mode) {
    const char bytes[]="import \"./坏 库\" as Bad\nprint(42)\n";
    const char bad[]="var bad =\n";
    const char *path=XR_DIAGNOSTIC_FIXTURE "/entry.xr";
    const XrSourceText input={mode?path:NULL,mode?NULL:"diagnostic-resources",mode?bytes:bad,
        mode?sizeof(bytes)-1:sizeof(bad)-1};
    if(mode==1)return xray_source_compile_file(owner->compiler,path,&owner->program,&owner->diagnostic);
    if(mode==3)return xray_source_compile_ast(owner->compiler,owner->ast,&owner->program,&owner->diagnostic);
    if(mode==4)return xray_source_repl_begin(owner->repl,&input,&owner->diagnostic);
    if(mode==5)return xray_source_repl_begin_file(owner->repl,path,&owner->diagnostic);
    if(mode==6)return xray_source_repl_begin_type(owner->repl,&input,&owner->diagnostic);
    return xray_source_compile_text(owner->compiler,&input,&owner->program,&owner->diagnostic);
}
static XrCompileResources *resources(DiagnosticOwners *owner) {
    return xr_source_compiler_resources(owner->repl?owner->repl->compiler:owner->compiler);
}
static void sticky(DiagnosticOwners *owner,unsigned mode,XrSourceStatus expected) {
    xray_source_diagnostic_free(owner->diagnostic);owner->diagnostic=NULL;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(resources(owner),&before)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=runtime_attempts;
    CHECK(fail_source(owner,mode)==expected && !owner->program && !owner->diagnostic);
    CHECK(xr_compile_resources_stats(resources(owner),&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(runtime_attempts==attempts && !memcmp(&before,&after,sizeof(before)));
}
static void close_owners(DiagnosticOwners *owner) {
    XrSourceEvent event={0};CHECK(xray_source_repl_drop(&owner->repl,&event)==XR_SOURCE_OK);
    xray_source_ast_free(owner->ast);xray_source_compiler_free(owner->compiler);
    xray_source_program_free(owner->program);xray_source_diagnostic_free(owner->diagnostic);
    *owner=(DiagnosticOwners){0};CHECK(!runtime_live && !runtime_bytes);
}
static void faults(unsigned mode) {
    size_t sites=0;
    for(size_t point=0;!point || point<=sites;++point) {
        DiagnosticOwners owner={0};runtime_fail_at=SIZE_MAX;
        CHECK(prepare(&owner,mode,&generous)==XR_SOURCE_OK);
        runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrSourceStatus status=fail_source(&owner,mode);
        CHECK(!owner.program);
        if(!point) {
            CHECK(status==XR_SOURCE_INVALID && owner.diagnostic);
            XrSourceDiagnosticRoot info={0};XrSourceDiagnosticReportInfo report_info={0};
            CHECK((xray_source_diagnostic_report(owner.diagnostic,&report_info)==XR_SOURCE_OK && report_info.roots==1 ? xray_source_diagnostic_root(owner.diagnostic,0,&info) : XR_SOURCE_BAD_STATE)==XR_SOURCE_OK);
            CHECK(info.primary.line==2 && info.primary.column==1 && !strcmp(info.message,"expected expression"));
            sites=runtime_attempts;CHECK(sites);
        } else {
            if(status!=XR_SOURCE_OUT_OF_MEMORY)fprintf(stderr,"mode=%u point=%zu status=%u\n",mode,point,(unsigned)status);
            CHECK(status==XR_SOURCE_OUT_OF_MEMORY && runtime_attempts>runtime_fail_at);
            runtime_fail_at=SIZE_MAX;sticky(&owner,mode,status);
        }
        runtime_fail_at=SIZE_MAX;close_owners(&owner);
    }
    printf("parser diagnostic mode=%u actual failure-operation allocations=%zu swept=%zu physical=0/0\n",mode,sites,sites);
}
static void budgets(unsigned mode) {
    DiagnosticOwners owner={0};XrCompileResourceStats required={0};
    CHECK(prepare(&owner,mode,&generous)==XR_SOURCE_OK && fail_source(&owner,mode)==XR_SOURCE_INVALID);
    CHECK(xr_compile_resources_stats(resources(&owner),&required)==XR_COMPILE_RESOURCE_OK);close_owners(&owner);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1);*bound=delta<0?*bound-1:*bound+(unsigned)delta;
        XrSourceStatus status=prepare(&owner,mode,&limits);
        if(status==XR_SOURCE_OK)status=fail_source(&owner,mode);
        if(delta<0) {
            if(status!=XR_SOURCE_BUDGET)fprintf(stderr,"mode=%u axis=%u status=%u\n",mode,axis,(unsigned)status);
            CHECK(status==XR_SOURCE_BUDGET && !owner.program);
            if(owner.compiler || owner.repl)sticky(&owner,mode,status);
        } else CHECK(status==XR_SOURCE_INVALID && owner.diagnostic && !owner.program);
        close_owners(&owner);
    }
    printf("parser diagnostic mode=%u exact axes=%llu/%llu/%llu minus/exact/plus physical=0/0\n",mode,
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
int main(void) {
    for(unsigned mode=0;mode<7;++mode){faults(mode);budgets(mode);}
    return 0;
}
