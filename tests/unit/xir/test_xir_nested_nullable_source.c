/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_nested_nullable_source.c - Source generic substitutions preserve every optional layer
 *
 * KEY CONCEPT:
 *   Source definitions, direct substitutions and transitive substitutions agree.
 */
#include "program/xr_xir_source_product.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_nested_nullable_compile_owner.h"
static void nested_source_case(const char *name,bool accept,bool hidden) {
    char directory[4096],path[4096];
    int length=snprintf(directory,sizeof(directory),"%s/%s",XR_NESTED_SOURCE_FIXTURES,name);
    CHECK(length>0 && (size_t)length<sizeof(directory));
    length=snprintf(path,sizeof(path),"%s/root.xr",directory);CHECK(length>0 && (size_t)length<sizeof(path));
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrXirSourceProductRequest request={{session,path,&authority,&owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    if(hidden){
        XrXirSourceResult result={0};XrXirArtifact *closed=NULL;
        CHECK(xr_xir_compile_source_check(&request.source,&result,NULL,NULL)==XR_XIR_OK);
        CHECK(result.checked && result.snapshot && xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL)==XR_XIR_OK);
        const XrXirTypes *types=xr_xir_compile_artifact_module(closed)->types;bool nested=false;
        for(uint32_t i=0;types && i<types->count;++i)
            if(types->nodes[i].kind==XR_XIR_TYPE_NULLABLE && xr_xir_type_is_nullable(types,types->nodes[i].element))nested=true;
        CHECK(nested);xr_xir_compile_artifact_free(closed);xr_xir_compile_source_result_free(&result);
    }
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=(accept?XR_XIR_OK:XR_XIR_BAD_TYPE))fprintf(stderr,"nested Source %s status=%u stage=%u: %s\n",
        name,status,diagnostic.stage,diagnostic.source.message);
    CHECK(status==(accept?XR_XIR_OK:XR_XIR_BAD_TYPE));
    if(accept){
        CHECK(product && xr_xir_compile_source_product_view(product)->complete);
        XrXirSourceProductPacketView packet={0};XrXirArtifact *closed=NULL;
        CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_read(&owner.context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
        const XrXirModule *module=xr_xir_compile_artifact_module(closed);
        bool nested=false;
        for(uint32_t i=0;module->types && i<module->types->count;++i){
            const XrXirTypeNode *node=&module->types->nodes[i];
            if (node->kind==XR_XIR_TYPE_NULLABLE && xr_xir_type_is_nullable(module->types,node->element)) nested=true;
        }
        CHECK(nested==(!strcmp(name,"definition")?false:strcmp(name,"single")!=0));
        xr_xir_compile_artifact_free(closed);
    }else if(hidden){
        CHECK(!product && diagnostic.stage==XR_XIR_SOURCE_PRODUCT_SPECIALIZE && diagnostic.xir.status==XR_XIR_BAD_TYPE);
        CHECK(diagnostic.snapshot && xr_xir_compile_source_snapshot_view(diagnostic.snapshot)->complete);
    }else{
        CHECK(!product && diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CHECK && diagnostic.source.status==XR_XIR_BAD_TYPE);
        CHECK(!strcmp(diagnostic.source.message,"nested nullable types are not admitted by source"));
    }
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);nested_nullable_compile_owner_free(&owner);
}
static uint64_t nested_source_quota(uint64_t work,XrXirStatus expected) {
    SourceFixtureOwner owner={0};
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,work};
    CHECK(xr_compile_resources_new(&limits,&owner.context.resources)==XR_COMPILE_RESOURCE_OK);
    owner.context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(owner.context.resources,&owner.baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(owner.context.resources,&session)==XR_COMPILER_SESSION_OK);
    const XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_NESTED_SOURCE_FIXTURES "/single"};
    const XrXirSourceProductRequest request={{session,XR_NESTED_SOURCE_FIXTURES "/single/root.xr",&authority,
        &owner.context,NULL,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if(status!=expected)fprintf(stderr,"nested quota=%llu status=%u stage=%u\n",(unsigned long long)work,status,diagnostic.stage);
    CHECK(status==expected && diagnostic.status==expected && (product!=NULL)==(status==XR_XIR_OK));
    xr_xir_compile_source_product_free(product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
    CHECK(stats.work<=work);nested_nullable_compile_owner_free(&owner);return stats.work;
}
int main(void){
    nested_source_case("definition",true,false);nested_source_case("single",true,false);
    nested_source_case("layer",true,false);nested_source_case("none",true,false);nested_source_case("reference",true,false);nested_source_case("inferred",true,false);
    nested_source_case("hidden_local",true,true);nested_source_case("hidden_transitive",true,true);
    uint64_t work=nested_source_quota(UINT64_C(128000000),XR_XIR_OK);CHECK(work>1);
    CHECK(nested_source_quota(work,XR_XIR_OK)==work);
    (void)nested_source_quota(work-1,XR_XIR_BUDGET);
    puts("Original eight Source inputs retain bytes; direct, inferred and hidden nested products accepted with exact layers PASS");return 0;
}
