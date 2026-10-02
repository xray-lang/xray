/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * check_owner_driver.c - Real command owner and typed input boundary
 */
#include "app/cli/xcli.h"
#include "app/cli/xcli_canonical_source.h"
#include "base/xwindows_utf8.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(90); } } while (0)
XR_FUNC int cmd_check(const XrCliInvocation *inv);
#if CHECK_OWNER_INJECTED
static size_t attempts, fail_at=SIZE_MAX, live;
static void *observed_malloc(size_t size) {
    if (attempts++==fail_at)return NULL;
    void *memory=xr_malloc(size); if(memory)++live;return memory;
}
static void observed_free(void *memory) { if(memory){CHECK(live);--live;}xr_free(memory); }
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_malloc
#define xr_free observed_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
static unsigned resource_calls, source_calls;
static XrCompileResourceLimits overridden;
static XrCompileResourceStats last_stats;
static size_t phases[3];
static XrCompileResources *original;
static XrCompileResourceStatus observed_new(const XrCompileResourceLimits *limits,XrCompileResources **output) {
    ++resource_calls;phases[0]=attempts;
    CHECK(limits->allocated_bytes==(UINT64_C(1)<<30) && limits->live_bytes==(UINT64_C(256)<<20) && limits->work==(UINT64_C(8)<<30));
    XrCompileResourceStatus status=xr_compile_resources_new(overridden.work?&overridden:limits,output);
    if(status==XR_COMPILE_RESOURCE_OK) original=*output;
    return status;
}
static void observed_release(XrCompileResources *owner) {
    CHECK(owner==original);
    CHECK(xr_compile_resources_stats(owner,&last_stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(owner);
}
static XrCliCompileSourceStatus observed_source(const XrCliCompileSourceRequest *request,
    XrXirSourceProduct **output,XrCliCompileSourceDiagnostic *diagnostic) {
    ++source_calls;CHECK(request->context->resources==original);phases[1]=attempts;
    return xr_cli_compile_source_build(request,output,diagnostic);
}
#define xr_compile_resources_new observed_new
#define xr_compile_resources_release observed_release
#define xr_cli_compile_source_build observed_source
#include "app/cli/xcmd_check.c"
#undef xr_compile_resources_new
#undef xr_compile_resources_release
#undef xr_cli_compile_source_build
static int checked_call(const XrCliInvocation *inv) {
    resource_calls=source_calls=0;attempts=0;original=NULL;
    DWORD handles_before=0,handles_after=0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    int result=cmd_check(inv);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after));
    CHECK(resource_calls==1 && live==0 && handles_before==handles_after);
    return result;
}
static void verify_boundaries(XrCliInvocation *inv) {
    CHECK(cmd_check(NULL)==4);
    CHECK(inv->positional_count==1);
    CHECK(checked_call(inv)==0);
    XrCompileResourceStats single=last_stats;
    size_t count=attempts, first_source=phases[1];
    if(xr_cli_opt_bool(&inv->options,"syntax-only")) {
        CHECK(!source_calls);
        for(size_t i=0;i<count;++i){fail_at=i;CHECK(checked_call(inv)==4);}
    } else {
        CHECK(source_calls==1);
        size_t selected[]={0,1,first_source};
        for(size_t i=0;i<sizeof(selected)/sizeof(*selected);++i){fail_at=selected[i];CHECK(checked_call(inv)==4);}
    }
    fail_at=SIZE_MAX;
    const char *twice[]={inv->positionals[0],inv->positionals[0]};
    const char **saved=inv->positionals;
    inv->positionals=twice;inv->positional_count=2;
    CHECK(checked_call(inv)==0);
    CHECK(last_stats.work>single.work && last_stats.allocated_bytes>single.allocated_bytes);
    overridden=xr_cli_compile_default_resource_limits();overridden.work=single.work;
    CHECK(checked_call(inv)==1);
    inv->positionals=saved;inv->positional_count=1;
    CHECK(checked_call(inv)==0);
    overridden.work=single.work-1;CHECK(checked_call(inv)==1);
    overridden=xr_cli_compile_default_resource_limits();
    overridden.allocated_bytes=single.allocated_bytes;CHECK(checked_call(inv)==0);
    --overridden.allocated_bytes;CHECK(checked_call(inv)==1);
    overridden=xr_cli_compile_default_resource_limits();
    overridden.live_bytes=single.peak_bytes;CHECK(checked_call(inv)==0);
    --overridden.live_bytes;CHECK(checked_call(inv)==1);
    overridden=(XrCompileResourceLimits){0};
    printf("CHECK_OWNER: one ledger, %zu real allocations, OOM boundaries, allocated/live/work exact/minus-one and cumulative two-file rejection, physical zero PASS\n",count);
}
#endif
static int drive(int argc,char **argv) {
    /* This consumes the real command specification at the typed invocation
     * boundary. Global parsing and dispatcher execution are separate gates. */
    const XrCliCommandSpec *spec=xr_cli_find_command("check");CHECK(spec);
    const XrCliCommandSpec *build=xr_cli_find_command("build");CHECK(build);
    for(int i=0;build->options[i].long_name;++i) CHECK(strcmp(build->options[i].long_name,"xi-opt")!=0);
    bool present[16]={0};const char *values[16]={0},*positionals[128];
    XrCliInvocation inv={0};inv.spec=spec;inv.options.spec=spec->options;
    inv.options.count=xr_cli_option_count(spec->options);CHECK(inv.options.count<=16);
    inv.options.present=present;inv.options.values=values;inv.positionals=positionals;
    for(int i=1;i<argc;++i) {
        if(argv[i][0]=='-' && argv[i][1]) {
            int found=-1;
            for(int j=0;j<inv.options.count;++j) {
                if((argv[i][1]=='-' && !strcmp(argv[i]+2,spec->options[j].long_name)) ||
                    (argv[i][1]==spec->options[j].short_name && !argv[i][2]))found=j;
            }
            if(found<0){fprintf(stderr,"unknown check option: %s\n",argv[i]);return 2;}
            present[found]=true;
        } else {CHECK(inv.positional_count<128);positionals[inv.positional_count++]=argv[i];}
    }
#if CHECK_OWNER_INJECTED
    verify_boundaries(&inv);return 0;
#else
    return cmd_check(&inv);
#endif
}
int wmain(int argc,wchar_t **wide_argv) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide_argv,&status);CHECK(argv);
    int result=drive(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
