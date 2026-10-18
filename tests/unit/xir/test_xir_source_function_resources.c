/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_function_resources.c - Complete fault closure for exact Source capacity
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "xir_source_requirement_value_program.h"
static XrXirStatus once(XrCompileResourceLimits caps,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&caps,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};context.limits.functions=15;
    const XrXirCompileContext original=context;
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirStatus status=XR_XIR_OK;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"exact-capacity",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,requirement_value_mixed_source,sizeof(requirement_value_mixed_source)-1};
    status=xr_xir_compile_source_check_text(&request,&text,&result,NULL,NULL);
done:
    if(stats)*stats=sdk_stats(resources);
    CHECK(!memcmp(&context,&original,sizeof(context)));
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    if(status==XR_XIR_OK){
        const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
        CHECK(module&&module->function_count==15&&view&&view->complete&&view->declaration_count>15);
    }else CHECK(!result.checked&&!result.snapshot);
    xr_xir_compile_source_result_free(&result);CHECK(!runtime_live&&!runtime_bytes);return status;
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==2||argc==4);
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    CHECK(once(caps,&stats)==XR_XIR_OK);size_t sites=runtime_attempts;
    if(!strcmp(argv[1],"baseline")){
        CHECK(argc==2);printf("{\"sites\":%zu,\"allocated\":%llu,\"peak\":%llu,\"work\":%llu,\"functions\":15,\"physical_live\":0,\"physical_bytes\":0}\n",sites,
            (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);return 0;
    }
    if(!strcmp(argv[1],"axes")){
        CHECK(argc==2);
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
            XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
            uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
            CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            CHECK(once(exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
        puts("Exact15 physical functions, all three resource axes minus1/exact/plus1, producer death and physical0/0 passed");return 0;
    }
    CHECK(!strcmp(argv[1],"sweep")&&argc==4);
    size_t first=(size_t)strtoull(argv[2],NULL,10),count=(size_t)strtoull(argv[3],NULL,10);CHECK(first<sites&&count);
    size_t end=count<sites-first?first+count:sites;
    for(size_t i=first;i<end;++i){
        runtime_attempts=0;runtime_fail_at=i;XrXirStatus status=once(caps,NULL);
        if(status!=XR_XIR_OUT_OF_MEMORY||runtime_attempts<=i)fprintf(stderr,"Source capacity FI=%zu actual=%u attempts=%zu\n",i,(unsigned)status,runtime_attempts);
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);
    }
    runtime_fail_at=SIZE_MAX;printf("Source capacity full fault range=%zu..%zu total=%zu physical0/0\n",first,end,sites);return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
