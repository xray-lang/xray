/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_nested_nullable.c - Whole Source optional-layer execution and failure owners
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_source_nested_nullable_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_source_nested_nullable_cases.h"
#include "xir_source_nested_nullable_pipeline.h"
static void sn_source_rejections(void) {
    static const char *const names[]={"default_type","default_generic","default_permission","cross_two_layers","unknown_null","default_permission_exact"};
    for (size_t n=0;n<sizeof(names)/sizeof(names[0]);++n) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
        CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
        char directory[4096],path[4096];
        int length=snprintf(directory,sizeof(directory),"%s/%s",XR_SOURCE_NESTED_FIXTURES,names[n]);
        CHECK(length>0 && (size_t)length<sizeof(directory));
        length=snprintf(path,sizeof(path),"%s/root.xr",directory);CHECK(length>0 && (size_t)length<sizeof(path));
        const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
        const XrXirSourceProductRequest request={{session,path,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL},
            {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
        XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
        if (status!=XR_XIR_BAD_TYPE) fprintf(stderr,"Source rejection %s status=%u stage=%u: %s\n",names[n],status,diagnostic.stage,diagnostic.source.message);
        CHECK(status==XR_XIR_BAD_TYPE && !product && diagnostic.status==status &&
            diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CHECK && diagnostic.source.status==status && !diagnostic.snapshot);
        if (n==5) CHECK(!strcmp(diagnostic.source.message,"value mutation requires a mutable named root"));
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
        CHECK(!runtime_live && !runtime_bytes);source_nested_compile_owner_free(&owner);
    }
}
static void sn_permission_control(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    const char *directory=XR_SOURCE_NESTED_FIXTURES "/default_permission_control";
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    const XrXirSourceProductRequest request={{session,XR_SOURCE_NESTED_FIXTURES "/default_permission_control/root.xr",
        &authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source writable-default control status=%u stage=%u %u:%u: %s\n",
        status,diagnostic.stage,diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
    CHECK(status==XR_XIR_OK && product && xr_xir_compile_source_product_view(product)->complete);
    CHECK(xr_xir_compile_source_product_verify(product,1048576,NULL)==XR_XIR_OK);
    XrXirSourceProductPacketView packet={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);uint32_t entry=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length==4 && !memcmp(function->name,"main",4)) {
            CHECK(entry==UINT32_MAX && !function->parameter_count && function->result==XR_XIR_I64);entry=f;
        }
    }
    CHECK(entry!=UINT32_MAX);xr_xir_compile_artifact_free(closed);XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK && program);
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);XrXirInstance *instance=NULL;XrXirInstanceConfig config;sn_config(&config);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);sn_expect(instance,entry,-1,41,false);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);source_nested_compile_owner_free(&owner);
}
static void sn_metadata_faults(void) {
    size_t sites=0;
    for (size_t pass=0;pass<=sites;++pass) {
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        source_nested_compile_attempts=0;source_nested_compile_injected=false;
        source_nested_compile_fail_at=pass?pass-1:SIZE_MAX;sn_metadata_profile=pass==0;
        XrXirProgram *program=NULL;uint32_t functions[SN_COUNT];
        XrXirStatus status=sn_build(&owner.context,NULL,&program,functions);
        size_t attempts=source_nested_compile_attempts;source_nested_compile_fail_at=SIZE_MAX;sn_metadata_profile=false;
        if (!pass) {
            CHECK(status==XR_XIR_OK && program);sites=attempts;
            fprintf(stderr,"Source nested metadata fresh scan baseline sites=%zu\n",sites);
            CHECK(sites && sites<20000);
        }
        else CHECK(source_nested_compile_injected && attempts>pass-1 && status==XR_XIR_OUT_OF_MEMORY && !program);
        xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
        source_nested_compile_owner_free(&owner);
    }
    printf("Source nested whole pipeline metadata OOM sites=%zu physical=0/0\n",sites);
}
static XrCompileResourceStats sn_metadata_quota(XrCompileResourceLimits limits,XrXirStatus expected) {
    SourceFixtureOwner owner={0};
    CHECK(xr_compile_resources_new(&limits,&owner.context.resources)==XR_COMPILE_RESOURCE_OK);
    owner.context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(owner.context.resources,&owner.baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;uint32_t functions[SN_COUNT];
    XrXirStatus status=sn_build(&owner.context,NULL,&program,functions);
    if (status!=expected) fprintf(stderr,"Source nested quota status=%u expected=%u\n",status,expected);
    CHECK(status==expected && (program!=NULL)==(status==XR_XIR_OK));
    xr_xir_compile_program_drop(program);CHECK(!runtime_live && !runtime_bytes);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocated_bytes<=limits.allocated_bytes && stats.peak_bytes<=limits.live_bytes && stats.work<=limits.work);
    source_nested_compile_owner_free(&owner);return stats;
}
static void sn_metadata_bounds(void) {
    XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResourceStats stats=sn_metadata_quota(caps,XR_XIR_OK);
    CHECK(stats.allocated_bytes>1 && stats.peak_bytes>1 && stats.work>1);
    const XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
    XrCompileResourceStats measured=sn_metadata_quota(exact,XR_XIR_OK);
    CHECK(measured.allocated_bytes==stats.allocated_bytes && measured.peak_bytes==stats.peak_bytes && measured.work==stats.work);
    caps=exact;--caps.allocated_bytes;(void)sn_metadata_quota(caps,XR_XIR_BUDGET);
    caps=exact;--caps.live_bytes;(void)sn_metadata_quota(caps,XR_XIR_BUDGET);
    caps=exact;--caps.work;(void)sn_metadata_quota(caps,XR_XIR_BUDGET);
}
#include "xir_source_nested_nullable_metadata_shards.h"
int main(int argc,char **argv) {
    if (sn_metadata_dispatch(argc,argv)) return 0;
    CHECK(argc==1 || argc==2);
    if (argc==2 && !strcmp(argv[1],"--metadata")) {
        sn_source_rejections();sn_permission_control();sn_metadata_faults();sn_metadata_bounds();puts("Source nested compiler OOM and three finite resource axes PASS");return 0;
    }
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgram *program=NULL;uint32_t functions[SN_COUNT];
    CHECK(sn_build(&owner.context,argc==2?argv[1]:NULL,&program,functions)==XR_XIR_OK);
    XrXirValue held[6]={{0}};
    if (argc==1) sn_program_cases(program,functions,held);
    else {
        XrXirInstance *instance=NULL;XrXirInstanceConfig config;sn_config(&config);
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);sn_vector(instance,functions,held);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);sn_retained(held);
    CHECK(!runtime_live && !runtime_bytes);source_nested_compile_owner_free(&owner);
    puts("Source nested whole Program fixed VM golden, producer destroyed, all values physically released PASS");return 0;
}
