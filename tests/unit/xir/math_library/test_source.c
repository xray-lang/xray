/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source.c - Complete math Checked ownership and source-free execution
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}} while(0)
#include "../xir_source_fixture_owner.h"
#include "execution.h"

static void math_query(const XrXirSourceView *view) {
    bool observed[28]={false};uint32_t count=0;
    CHECK(view && view->complete);
    for (uint32_t i=0;i<view->declaration_count;++i) {
        const XrXirSourceDeclaration *row=&view->declarations[i];
        if (row->native_identity<UINT32_C(0x10000001) || row->native_identity>UINT32_C(0x1000001c)) continue;
        uint32_t ordinal=row->native_identity-UINT32_C(0x10000001);
        CHECK(!observed[ordinal] && row->kind==XR_XIR_SOURCE_INTRINSIC && !row->exported && row->signature);
        CHECK(row->range.module<view->module_count && row->range.line>0 && row->range.column>0);
        CHECK(!strcmp(view->modules[row->range.module].path,"stdlib/defs/core.def"));
        CHECK(!strncmp(row->name,"__",2));observed[ordinal]=true;++count;
    }
    CHECK(count==28);
}
static XrXirArtifact *math_source(const XrXirCompileContext *context,const char *root,
    const char *name,bool library,const XrXirLibraryCatalog *catalog) {
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/%s",root,name)>0);
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={library?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_SCRIPT,
        library?"math":NULL,root};
    XrXirSourceRequest request={session,path,&authority,context,root,NULL,
        library?XR_XIR_LIBRARY:XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    fprintf(stderr,"Math Source %s status%u line%d column%d work%llu allocated%llu live%llu: %s\n",
        name,status,diagnostic.line,diagnostic.column,(unsigned long long)stats.work,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes,diagnostic.message);
    CHECK(status==XR_XIR_OK);
    if (library) math_query(xr_xir_compile_source_snapshot_view(result.snapshot));
    XrXirArtifact *artifact=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);return artifact;
}
static void math_inventory(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    uint32_t functions=0,constants=0;
    CHECK(module && module->declarations && module->declarations->module_count==1);
    for (uint32_t i=0;i<module->function_count;++i) functions+=module->declarations->functions[i].exported;
    for (uint32_t i=0;i<module->declarations->slot_count;++i) {
        const XrXirSlot *slot=&module->declarations->slots[i];
        CHECK(!slot->mutable);constants+=slot->exported;
    }
    CHECK(functions==37 && constants==14);
}
static void math_folded(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint32_t exported=0;
    for (uint32_t i=0;i<module->function_count;++i) {
        const XrXirFunctionIdentity *identity=&module->declarations->functions[i];
        if (!identity->exported || identity->module!=module->declarations->root_module) continue;
        const XrXirFunction *function=&module->functions[i];bool constant=false;
        CHECK(function->result==XR_XIR_F64 && !function->parameter_count);
        for (uint32_t n=0;n<function->instruction_count;++n) {
            const XrXirInstruction *op=&function->instructions[n];
            constant|=op->op==XR_XIR_CONST_FLOAT && op->type==XR_XIR_F64 && op->immediate==INT64_C(0x4022000000000000);
            CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_SLOT_LOAD &&
                !(op->op>=XR_XIR_MATH_UNARY_F64 && op->op<=XR_XIR_RANDOM_I64));
        }
        CHECK(constant);++exported;
    }
    CHECK(exported==1);
}
int main(int argc,char **argv) {
    CHECK(argc==2 || argc==3);const char *root=argv[1];
    SourceFixtureOwner producer_owner={0};source_fixture_owner_new(&producer_owner);
    XrXirArtifact *producer=math_source(&producer_owner.context,root,"math/math.xr",true,NULL);
    math_inventory(producer);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(producer);
    /* These are independent compiler invocations. Destroy the complete producer
     * ledger before consumer checking, retaining only receiver-owned metadata. */
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_STDLIB,"math",root};
    char logical[]="math/math.xr";
    XrXirLibraryModuleInput binding={.logical_path=logical,.authority=authority};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(&owner.context,&input,1,&catalog);
    if (status!=XR_XIR_OK) {
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(owner.context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
        fprintf(stderr,"Math Catalog status%u work%llu allocated%llu live%llu\n",status,
            (unsigned long long)stats.work,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.live_bytes);
    }
    CHECK(status==XR_XIR_OK);
    SourceFixtureOwner ctfe_owner={0};source_fixture_owner_new(&ctfe_owner);
    XrXirLibraryCatalog *ctfe_catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(&ctfe_owner.context,&input,1,&ctfe_catalog)==XR_XIR_OK);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    source_fixture_owner_free(&producer_owner);
    memset(logical,0,sizeof(logical));memset(&binding,0,sizeof(binding));memset(&input,0,sizeof(input));
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/math/math.xr",root)>0 && !remove(path));
    XrXirArtifact *received=math_source(&owner.context,root,"root.xr",false,catalog);
    XrXirArtifact *folded=math_source(&ctfe_owner.context,root,"ctfe.xr",false,ctfe_catalog);
    math_folded(folded);xr_xir_compile_artifact_free(folded);
    xr_xir_compile_library_catalog_free(ctfe_catalog);source_fixture_owner_free(&ctfe_owner);
    xr_xir_compile_library_catalog_free(catalog);
    if (argc==3) {
        CHECK(xr_xir_compile_checked_write(received,&packet,NULL)==XR_XIR_OK);
        CHECK(snprintf(path,sizeof(path),"%s.chk",argv[2])>0);
        FILE *file=fopen(path,"wb");CHECK(file);
        CHECK(fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));
        xr_xir_compile_checked_packet_free(&packet);
    }
    math_vm(&owner.context,received,argc==3?argv[2]:NULL);
    xr_xir_compile_artifact_free(received);source_fixture_owner_free(&owner);
    puts("Complete37 functions/14 constants, governed28 query facts, owned Catalog without source and actual folded/VM math PASS");
    return 0;
}
