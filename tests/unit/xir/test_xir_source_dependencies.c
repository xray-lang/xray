/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_dependencies.c - Real Source dependency owners and separately frozen resource probes
 */
#include "app/cli/xcli_canonical_source.h"
#include "app/cli/xcli_dependency_output.h"
#include "module/xlockfile.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_compile_owner.h"

static XrCliCompileSourceStatus raw_status(XrXirStatus status) {
    if (status == XR_XIR_OK) return XR_CLI_COMPILE_SOURCE_OK;
    if (status == XR_XIR_BUDGET) return XR_CLI_COMPILE_SOURCE_BUDGET;
    if (status == XR_XIR_OUT_OF_MEMORY) return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    return XR_CLI_COMPILE_SOURCE_REJECTED;
}
static XrCliCompileSourceStatus io_status(XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return XR_CLI_COMPILE_SOURCE_OK;
    if (status == XR_OS_IO_BUDGET) return XR_CLI_COMPILE_SOURCE_BUDGET;
    if (status == XR_OS_IO_OUT_OF_MEMORY) return XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    return XR_CLI_COMPILE_SOURCE_REJECTED;
}
static XrCliCompileSourceStatus build(const XrXirCompileContext *context, char **argv,
    XrXirSourceProduct **output) {
    if (!strcmp(argv[5],"catalog")) {
        XrCliCompileSourceRequest request={context,argv[2],argv[3],NULL,
            xr_cli_compile_default_manifest_limits(),{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
        XrCliCompileSourceDiagnostic diagnostic={0};
        XrCliCompileSourceStatus status=xr_cli_compile_source_build(&request,output,&diagnostic);
        if (status != XR_CLI_COMPILE_SOURCE_OK) fprintf(stderr,"build status=%u stage=%u\n",status,diagnostic.stage);
        xr_cli_compile_source_diagnostic_free(&diagnostic);
        return status;
    }
    XrCompilerSession *session=NULL; XrLockfile *lock=NULL;
    XrCompilerSessionStatus ss=xr_compile_session_new(context->resources,&session);
    XrCliCompileSourceStatus status=ss==XR_COMPILER_SESSION_OK ? XR_CLI_COMPILE_SOURCE_OK :
        ss==XR_COMPILER_SESSION_BUDGET ? XR_CLI_COMPILE_SOURCE_BUDGET : XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    XrOsIoPolicy policy=xr_compile_io_policy(context->resources);
    if (status==XR_CLI_COMPILE_SOURCE_OK && strcmp(argv[4],"-")) {
        status=io_status(xr_lockfile_new_owned(&policy,&lock));
        if (status==XR_CLI_COMPILE_SOURCE_OK)
            status=io_status(xr_lockfile_add_package_owned(lock,"fixture/math","1.0.0","",argv[4]));
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"fixture",argv[1]};
    XrXirSourceProductRequest request={{session,argv[2],&authority,context,argv[3],lock,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProductDiagnostic diagnostic={0};
    if (status==XR_CLI_COMPILE_SOURCE_OK)
        status=raw_status(xr_xir_compile_source_product_build(&request,output,&diagnostic));
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_lockfile_free_owned(lock); xr_compile_session_free(session);
    return status;
}
static void report(const XrXirSourceProduct *product,const char *prefix) {
    const XrXirSourceDependencies *facts=xr_xir_compile_source_product_dependencies(product);
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);
    CHECK(facts && view && facts->count && facts->entry<facts->count && facts->count<=view->module_count);
    printf("GRAPH count=%u entry=%u viewmodules=%u\n",facts->count,facts->entry,view->module_count);
    for (uint32_t i=0;i<facts->count;++i) {
        const XrXirSourceDependency *row=&facts->entries[i];
        CHECK(row->module<view->module_count && view->modules[row->module].identity);
        printf("ROW %u module=%u kind=%u imports=",i,row->module,row->kind);
        for (uint32_t j=0;j<row->import_count;++j) printf("%s%u",j ? "," : "",row->imports[j]);
        printf(" path=%s\n",row->path);
    }
    static const char *suffix[]={".sh",".json",".list"};
    for (unsigned i=0;i<3;++i) {
        char path[4096];int n=snprintf(path,sizeof(path),"%s%s",prefix,suffix[i]);
        CHECK(n>0 && (size_t)n<sizeof(path)); FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(deps_emit(file,facts,(OutputFormat)i));CHECK(fclose(file)==0);
    }
}
int main(int argc,char **argv) {
    /* root entry stdlib checksum|- raw|catalog outprefix expected fail-index|none alloc live work [retry-expected] */
    CHECK(argc==12 || argc==13);
    XrCompileResourceLimits caps={strtoull(argv[9],NULL,10),strtoull(argv[10],NULL,10),strtoull(argv[11],NULL,10)};
    unsigned expected=(unsigned)strtoul(argv[7],NULL,10);
    source_fixture_compile_fail_at=!strcmp(argv[8],"none") ? SIZE_MAX : (size_t)strtoull(argv[8],NULL,10);
    XrCompileResources *resources=NULL;
    XrCompileResourceStatus rs=xr_compile_resources_new(&caps,&resources);
    XrCliCompileSourceStatus status=rs==XR_COMPILE_RESOURCE_OK ? XR_CLI_COMPILE_SOURCE_OK :
        rs==XR_COMPILE_RESOURCE_BUDGET ? XR_CLI_COMPILE_SOURCE_BUDGET : XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrXirSourceProduct *product=NULL;
    if (status==XR_CLI_COMPILE_SOURCE_OK) status=build(&context,argv,&product);
    CHECK((unsigned)status==expected);
    if (status!=XR_CLI_COMPILE_SOURCE_OK) CHECK(!product);
    if (source_fixture_compile_fail_at!=SIZE_MAX) CHECK(source_fixture_compile_injected && status==XR_CLI_COMPILE_SOURCE_OUT_OF_MEMORY);
    source_fixture_compile_fail_at=SIZE_MAX;
    XrCompileResourceStats stats={0};
    if (resources) CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    printf("CENSUS attempts=%zu allocated=%llu peak=%llu work=%llu live=%llu status=%u injected=%u\n",
        source_fixture_compile_attempts,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,
        (unsigned long long)stats.work,(unsigned long long)stats.live_bytes,status,(unsigned)source_fixture_compile_injected);
    if (argc==13) {
        CHECK(status==XR_CLI_COMPILE_SOURCE_OK); xr_xir_compile_source_product_free(product);product=NULL;
        status=build(&context,argv,&product);CHECK((unsigned)status==(unsigned)strtoul(argv[12],NULL,10));
        if (status!=XR_CLI_COMPILE_SOURCE_OK) CHECK(!product);
        printf("RETRY status=%u\n",status);
    }
    xr_compile_resources_release(resources);
    /* Session, Catalog, lockfile and the caller ledger reference are dead here. */
    if (product) report(product,argv[6]);
    xr_xir_compile_source_product_free(product);
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    puts("PHYSICAL blocks=0 bytes=0");return 0;
}
