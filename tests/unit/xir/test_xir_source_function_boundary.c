/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_function_boundary.c - Physical function limits survive effect analysis
 *
 * KEY CONCEPT:
 *   Analysis-only contexts do not create new executable declarations. Exact
 *   structural function limits still reject a genuinely larger source body.
 */
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
static const char *const texts[]={
    "fn identity<T>(value:T)->T{return value}\nvar result=identity(7)\n",
    "fn first()->i64{return 1}\nfn second()->i64{return 2}\nvar result=first()+second()\n"
};
static XrXirStatus once(unsigned mode,uint32_t limit,uint32_t *count) {
    const XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=sdk_ledger(&caps);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    context.limits.functions=limit;XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"function-boundary",NULL};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText text={NULL,texts[mode],strlen(texts[mode])};XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&result,&diagnostic,NULL);
    if(status==XR_XIR_OK){const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);CHECK(module&&result.snapshot);*count=module->function_count;}
    else CHECK(!result.checked&&!result.snapshot);
    XrCompileResourceStats stats=sdk_stats(resources);
    printf("Source exact function boundary mode=%u limit=%u status=%u functions=%u work=%llu diagnostic=%s\n",mode,limit,(unsigned)status,*count,(unsigned long long)stats.work,diagnostic.message);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);return status;
}
static int run(int argc,char **argv) {
    CHECK(argc==1);(void)argv;(void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    for(unsigned mode=0;mode<2;++mode){
        uint32_t count=0,observed=0;CHECK(once(mode,xr_xir_compile_default_limits().functions,&count)==XR_XIR_OK&&count>1);
        CHECK(once(mode,count-1,&observed)==XR_XIR_BUDGET&&!observed);
        CHECK(once(mode,count,&observed)==XR_XIR_OK&&observed==count);observed=0;
        CHECK(once(mode,count+1,&observed)==XR_XIR_OK&&observed==count);
    }
    return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
