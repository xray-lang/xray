/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_directory_resources.c - Observe actual directory allocation and work
 */
#include "base/xmalloc.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
static size_t attempts,fail_at=SIZE_MAX,live;
static void *allocate(size_t size) {
    if(attempts++==fail_at)return NULL;
    void *pointer=xr_malloc(size);CHECK(pointer);++live;return pointer;
}
static void release(void *pointer) { if(pointer) {CHECK(live);--live;xr_free(pointer);} }
#undef xr_malloc
#undef xr_free
#define xr_malloc(size) allocate(size)
#define xr_free(pointer) release(pointer)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#include "program/xr_xir_source_product.c"
static XrCompileResources *ledger(const XrCompileResourceLimits *limits) {
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(limits,&resources)==XR_COMPILE_RESOURCE_OK);return resources;
}
static XrXirStatus directory(const XrCompileResourceLimits *limits,size_t failed,XrCompileResourceStats *stats) {
    CHECK(!live);attempts=0;fail_at=SIZE_MAX;
    XrCompileResources *resources=ledger(limits);fail_at=failed;
    const XrXirFunctionIdentity identities[]={{.test_role=XR_XIR_TEST_ROLE_TEST},
        {.module=1,.test_role=XR_XIR_TEST_ROLE_TEST},{0}};
    const XrXirFunction functions[]={{.name="A",.name_length=1},{.name="A",.name_length=1},{.name="plain",.name_length=5}};
    const XrXirDeclarations declarations={.functions=identities,.root_module=0};
    const XrXirModule module={.functions=functions,.function_count=3,.declarations=&declarations};
    XrXirSourceProduct product={.context={resources,xr_xir_compile_default_limits()}};
    XrXirStatus status=source_product_tests(&product,&module);
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    if(status==XR_XIR_OK) CHECK(product.tests.count==1 && !strcmp(product.tests.entries[0].name,"A"));
    else CHECK(!product.tests.entries && !product.tests.count);
    xr_compile_resources_free((void *)product.tests.entries);xr_compile_resources_release(resources);CHECK(!live);
    fail_at=SIZE_MAX;return status;
}
static void directory_gates(void) {
    const XrCompileResourceLimits unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResourceStats exact={0},current={0};
    CHECK(directory(&unlimited,SIZE_MAX,&exact)==XR_XIR_OK);
    /* Two actual identity-copy passes, one selected function copy each pass,
     * calloc zeroes the whole block, then name/terminator and row are copied.
     * Ledger creation and the directory block each perform one allocation. */
    uint64_t block=sizeof(XrXirSourceTestEntry)+2;
    uint64_t expected=sizeof(uint32_t)+2*3*sizeof(XrXirFunctionIdentity)+2*sizeof(XrXirFunction)+
        2+block+2+sizeof(XrXirSourceTestEntry)+sizeof(XrXirSourceTests);
    CHECK(exact.work==expected && attempts==2);
    CHECK(directory(&unlimited,1,&current)==XR_XIR_OUT_OF_MEMORY);
    for(uint64_t work=1;work<expected;++work) {
        XrCompileResourceLimits limits=unlimited;limits.work=work;
        CHECK(directory(&limits,SIZE_MAX,&current)==XR_XIR_BUDGET);
    }
    for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=unlimited;
        if(axis==0)limits.allocated_bytes=exact.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=exact.peak_bytes-minus;
        else limits.work=expected-minus;
        CHECK(directory(&limits,SIZE_MAX,&current)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
    }
    printf("directory independent work=%llu all lower work limits, one real OOM; three-axis exact/minus1 physical0\n",(unsigned long long)expected);
}
static XrXirStatus source_once(const char *root,const char *stdlib,size_t failure,
    const XrCompileResourceLimits *limits,XrCompileResourceStats *stats,size_t *count) {
    CHECK(!live);attempts=0;fail_at=SIZE_MAX;XrCompileResources *resources=ledger(limits);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    char path[1024];CHECK(snprintf(path,sizeof(path),"%s/basic.xr",root)>0);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceProductRequest request={{session,path,&authority,&context,stdlib,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};fail_at=failure;
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    *count=attempts;CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    if(status==XR_XIR_OK) CHECK(product && xr_xir_compile_source_product_tests(product)->count==7);
    else CHECK(!product);
    if(failure!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.stage==XR_XIR_SOURCE_PRODUCT_FACTS);
    fail_at=SIZE_MAX;xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_xir_compile_source_product_free(product);xr_compile_session_free(session);xr_compile_resources_release(resources);CHECK(!live);
    return status;
}
int main(int argc,char **argv) {
    CHECK(argc==3);directory_gates();XrCompileResourceLimits unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResourceStats exact={0},actual={0};size_t count=0,ignored=0;
    CHECK(source_once(argv[1],argv[2],SIZE_MAX,&unlimited,&exact,&count)==XR_XIR_OK);
    CHECK(source_once(argv[1],argv[2],count-1,&unlimited,&actual,&ignored)==XR_XIR_OUT_OF_MEMORY);
    for(unsigned axis=0;axis<3;++axis) for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=unlimited;
        if(axis==0)limits.allocated_bytes=exact.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=exact.peak_bytes-minus;
        else limits.work=exact.work-minus;
        CHECK(source_once(argv[1],argv[2],SIZE_MAX,&limits,&actual,&ignored)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
    }
    puts("actual Source final directory OOM + cumulative exact/minus1; all compiler blocks physically released");return 0;
}
