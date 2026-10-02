/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * run_owner_driver.c - Command input, Source admission and host consumer
 */
#include "app/cli/xcli.h"
#include "app/cli/xcli_source_paths.h"
#include "execution/xr_xir_host_cli.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(90); } } while (0)
XR_FUNC int cmd_run(const XrCliInvocation *inv);

#if RUN_OWNER_INJECTED
#include "../../xir/xir_runtime_allocations.h"
#include "base/xcompile_resources.c"
#include "xir/xxir_format.c"
#include "execution/xr_xir_host_execution.c"
#include "execution/xr_xir_host_cli.c"
static size_t phase_allocations[5];
static unsigned resource_calls;
static bool budget_failure;
static XrCompileResourceStatus observed_resources(const XrCompileResourceLimits *limits,
    XrCompileResources **output) {
    ++resource_calls;phase_allocations[0]=runtime_attempts;
    CHECK(limits->allocated_bytes==(UINT64_C(1)<<30) && limits->live_bytes==(UINT64_C(256)<<20) &&
        limits->work==(UINT64_C(8)<<30));
    XrCompileResourceLimits actual=*limits;
    if(budget_failure)actual.work=1;
    return xr_compile_resources_new(&actual,output);
}
static XrCliCompileSourceStatus observed_paths(XrCompileResources *resources,const char *entry,
    XrCliSourcePaths *output,XrCliSourcePathsDiagnostic *diagnostic) {
    phase_allocations[1]=runtime_attempts;
    return xr_cli_compile_source_paths(resources,entry,output,diagnostic);
}
static XrCliCompileSourceStatus observed_source(const XrCliCompileSourceRequest *request,
    XrXirSourceProduct **output,XrCliCompileSourceDiagnostic *diagnostic) {
    phase_allocations[2]=runtime_attempts;
    CHECK(request->manifest_limits.input_bytes==1048576 && request->manifest_limits.depth==64);
    return xr_cli_compile_source_build(request,output,diagnostic);
}
static XrXirStatus observed_take(XrXirSourceProduct *product,XrXirProgram **output) {
    phase_allocations[3]=runtime_attempts;
    return xr_xir_compile_source_product_vm_take(product,output);
}
static int observed_program(XrXirProgram *program,uint32_t entry) {
    phase_allocations[4]=runtime_attempts;
    return xr_xir_host_program_main(program,entry);
}
#define xr_compile_resources_new observed_resources
#define xr_cli_compile_source_paths observed_paths
#define xr_cli_compile_source_build observed_source
#define xr_xir_compile_source_product_vm_take observed_take
#define xr_xir_host_program_main observed_program
#include "app/cli/xcmd_run.c"
#undef xr_compile_resources_new
#undef xr_cli_compile_source_paths
#undef xr_cli_compile_source_build
#undef xr_xir_compile_source_product_vm_take
#undef xr_xir_host_program_main

static void verify_boundaries(const XrCliInvocation *inv) {
    CHECK(cmd_run(NULL)==4);
    runtime_attempts=0;resource_calls=0;
    CHECK(cmd_run(inv)==0 && !runtime_live && !runtime_bytes && resource_calls==1);
    size_t phases[5];memcpy(phases,phase_allocations,sizeof(phases));
    for(unsigned i=0;i<5;++i) {
        runtime_attempts=0;resource_calls=0;runtime_fail_at=phases[i];
        CHECK(cmd_run(inv)==4 && runtime_attempts>phases[i] && resource_calls==1);
        runtime_fail_at=SIZE_MAX;CHECK(!runtime_live && !runtime_bytes);
    }
    budget_failure=true;resource_calls=0;
    CHECK(cmd_run(inv)==1 && !runtime_live && !runtime_bytes && resource_calls==1);
    budget_failure=false;
    printf("RUN_OWNER: real default policy, five phase OOM boundaries, one ledger, budget and physical zero PASS\n");
}
#endif

static int run_driver(int argc,char **argv) {
    /* Inputs arrive at the same typed boundary used by the public dispatcher.
     * Parsing global options and dispatching other commands are outside this gate. */
    XrCliInvocation invocation={0};
    const char *positionals[128];CHECK(argc<=129);
    for(int i=1;i<argc;++i)positionals[i-1]=argv[i];
    invocation.positionals=positionals;invocation.positional_count=argc-1;
    for(int i=1;i<argc;++i) if(strcmp(argv[i],"--")==0) {
        invocation.positional_count=i-1;
        invocation.passthrough_argv=argv+i+1;invocation.passthrough_argc=argc-i-1;
        break;
    }
#if RUN_OWNER_INJECTED
    verify_boundaries(&invocation);
    return 0;
#else
    return cmd_run(&invocation);
#endif
}

int wmain(int argc,wchar_t **wide_argv) {
    XrWinPathStatus status;
    char **argv=xr_win_utf16_arguments(argc,wide_argv,&status);
    CHECK(argv);
    int result=run_driver(argc,argv);
    xr_win_utf8_arguments_free(argc,argv);
    return result;
}
