/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_faults.c - Complete governed math Source allocation failures
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xir/xxir_source.h"
#include "toolchain/xcompiler_session.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_library_compile_owner.h"
static XrXirStatus math_source_operation(const XrXirCompileContext *context,void *fixture) {
    const char *root=fixture;char path[4096];CHECK(snprintf(path,sizeof(path),"%s/math/math.xr",root)>0);
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_OUT_OF_MEMORY ?
        XR_XIR_OUT_OF_MEMORY : created==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"math",root};
    XrXirSourceRequest request={session,path,&authority,context,root,NULL,XR_XIR_LIBRARY,NULL};
    XrXirSourceResult result={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,NULL,NULL);
    xr_compile_session_free(session);
    if (status==XR_XIR_OK) {
        CHECK(result.checked && result.snapshot);
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);uint32_t count=0;
        CHECK(view && view->complete);
        for (uint32_t i=0;i<view->declaration_count;++i) {
            const XrXirSourceDeclaration *row=&view->declarations[i];
            if (row->native_identity<UINT32_C(0x10000001) || row->native_identity>UINT32_C(0x1000001c)) continue;
            CHECK(row->signature && row->name && !strncmp(row->name,"__",2) && !row->exported);++count;
        }
        CHECK(count==28);
    } else CHECK(!result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);return status;
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    library_compile_operation_cases("Complete governed math Source",math_source_operation,argv[1]);
    library_compile_observer_free();
    puts("Complete37 function/14 constant Source physical FI, surviving28 query facts and three exact/minus-one budgets PASS");
    return 0;
}
