/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_state_typed_values_source.c - Source and Checked Library private state ownership
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_library_catalog.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_library_compile_owner.h"
#include "xir/xxir_source_query.c"
#include "xir/xxir_type_inference.c"
#include "xir/xxir_source.c"
static size_t library_state_typed_values_source_calls,library_state_typed_values_checked_writes;
static size_t library_state_typed_values_catalog_builds,library_state_typed_values_scans,library_state_typed_values_verifications;
static void library_state_typed_values_observe_cases(const char *name,LibraryCompileOperation operation,void *fixture) {
    ++library_state_typed_values_scans;library_compile_operation_cases(name,operation,fixture);
}
static XrXirStatus library_state_typed_values_observe_source(const XrXirSourceRequest *request,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic,char **failure_path) {
    ++library_state_typed_values_source_calls;
    return xr_xir_compile_source_check(request,output,diagnostic,failure_path);
}
static XrXirStatus library_state_typed_values_observe_write(const XrXirArtifact *artifact,
    XrXirCheckedPacket *output,XrXirDiagnostic *diagnostic) {
    ++library_state_typed_values_checked_writes;
    return xr_xir_compile_checked_write(artifact,output,diagnostic);
}
static XrXirStatus library_state_typed_values_observe_catalog(const XrXirCompileContext *context,
    const XrXirLibraryInput *inputs,size_t count,XrXirLibraryCatalog **output) {
    ++library_state_typed_values_catalog_builds;
    return xr_xir_compile_library_catalog_new_v2(context,inputs,count,output);
}
static XrXirStatus library_state_typed_values_observe_verify(const XrXirArtifact *artifact,XrXirDiagnostic *diagnostic) {
    ++library_state_typed_values_verifications;return xr_xir_compile_artifact_verify(artifact,diagnostic);
}
/* These test counters observe real API calls after their implementations are
 * defined. They change no Source or Checked admission path. */
#define xr_xir_compile_source_check library_state_typed_values_observe_source
#define xr_xir_compile_checked_write library_state_typed_values_observe_write
#define xr_xir_compile_library_catalog_new_v2 library_state_typed_values_observe_catalog
#define xr_xir_compile_artifact_verify library_state_typed_values_observe_verify
#define library_compile_operation_cases library_state_typed_values_observe_cases
#include "xir_library_source_owner.h"
#include "xir_library_state_typed_values_runtime.h"
#include "xir_library_state_typed_values_source_cases.h"

static void library_state_typed_values_query(const XrXirSourceView *view,XrXirLinkageKind kind) {
    static const char *const names[]={"record","values","pair","optional","callback","packet","object"};
    uint32_t found=0,own=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *declaration=&view->declarations[d];
        if (declaration->kind!=XR_XIR_SOURCE_BINDING || !declaration->name || !declaration->mutable) continue;
        if (kind==XR_XIR_PROGRAM && !strcmp(declaration->name,"own")) {
            CHECK(declaration->type.known && declaration->type.type==XR_XIR_I64);++own;
        }
        if (kind!=XR_XIR_LIBRARY) continue;
        for (uint32_t n=0;n<7;++n) if (!strcmp(declaration->name,names[n])) {
            CHECK(!(found&(1u<<n)) && declaration->type.known &&
                !xr_xir_type_is_cell(view->types,declaration->type.type));
            found|=1u<<n;
        }
    }
    CHECK(kind==XR_XIR_LIBRARY ? found==127 : own==1);
}
static XrXirArtifact *library_state_typed_values_check(const XrXirCompileContext *context,const char *path,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    if (kind==XR_XIR_PROGRAM) library_state_typed_values_source_negatives(&request);
    else {
        library_state_typed_values_negative(&request,"export_var.xr",XR_XIR_LIBRARY);
        library_state_typed_values_negative(&request,"export_destructure.xr",XR_XIR_LIBRARY);
        library_state_typed_values_negative(&request,"non_sendable.xr",XR_XIR_LIBRARY);
        library_state_typed_values_negative(&request,"const_write.xr",XR_XIR_LIBRARY);
    }
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"state source %s status%u reason%s\n",path,status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view && view->complete);
    library_state_typed_values_query(view,kind);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);return owned;
}
static void library_state_typed_values_emit_packet(const XrXirCheckedPacket *packet,const char *path) {
    if (!path) return;
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(packet->bytes,1,packet->length,file)==packet->length && !fclose(file));
}
static void library_state_typed_values_inventory(const XrXirArtifact *owned) {
    const XrXirModule *module=xr_xir_compile_artifact_module(owned);
    CHECK(module->declarations->slot_count==8);
    uint32_t library_slots=0,nominals=0,arrays=0,tuples=0,nullables=0,callables=0;
    for (uint32_t s=0;s<module->declarations->slot_count;++s) {
        const XrXirSlot *slot=&module->declarations->slots[s];
        CHECK(slot->mutable && xr_xir_type_is_cell(module->types,slot->type));
        XrXirType logical=xr_xir_cell_element(module->types,slot->type);
        if (slot->module==module->declarations->root_module) {CHECK(logical==XR_XIR_I64);continue;}
        const XrXirTypeNode *node=xr_xir_type_node(module->types,logical);CHECK(node);
        ++library_slots;nominals+=node->kind==XR_XIR_TYPE_NOMINAL;arrays+=node->kind==XR_XIR_TYPE_ARRAY;
        tuples+=node->kind==XR_XIR_TYPE_TUPLE;nullables+=node->kind==XR_XIR_TYPE_NULLABLE;
        callables+=node->kind==XR_XIR_TYPE_CALLABLE;
    }
    CHECK(library_slots==7 && nominals==3 && arrays==1 && tuples==1 && nullables==1 && callables==1);
}
static void library_state_typed_values_program(const XrXirCompileContext *context,XrXirLibraryCatalog *catalog,const char *path) {
    XrXirArtifact *owned=library_state_typed_values_check(context,XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,catalog);
    library_state_typed_values_inventory(owned);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    LibraryPacketFixture replay={packet.bytes,packet.length,NULL};
    library_compile_operation_cases(catalog ? "State Catalog whole Checked reader" : "State Source whole Checked reader",library_packet_operation,&replay);
    library_state_typed_values_emit_packet(&packet,path);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_state_typed_values_run(context,owned);
}
/* Build outputs use the same normal Source and Checked owners. Full fault
 * replay belongs to the separate test entry, so generation never invokes it. */
static void library_state_typed_values_write_checked(const XrXirCompileContext *context,
    const char *source_path,const char *catalog_path) {
    XrXirCheckedPacket source={0},library={0},linked={0};
    XrXirArtifact *owned=library_state_typed_values_check(context,XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,NULL);
    library_state_typed_values_inventory(owned);
    CHECK(xr_xir_compile_checked_write(owned,&source,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    owned=library_state_typed_values_check(context,XR_SOURCE_FIXTURES "/state.xr",XR_XIR_LIBRARY,NULL);
    CHECK(xr_xir_compile_checked_write(owned,&library,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    char logical_path[16]="state.xr";
    XrXirLibraryModuleInput binding={.logical_path=logical_path,.authority=authority};
    XrXirLibraryInput input={.packet=library.bytes,.length=library.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog)==XR_XIR_OK);
    memset(library.bytes,0,library.length);xr_xir_compile_checked_packet_free(&library);
    memset(logical_path,0,sizeof(logical_path));memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    owned=library_state_typed_values_check(context,XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,catalog);
    library_state_typed_values_inventory(owned);xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_xir_compile_checked_write(owned,&linked,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    CHECK(library_state_typed_values_source_calls==15 && library_state_typed_values_checked_writes==3 &&
        library_state_typed_values_catalog_builds==1 && library_state_typed_values_verifications==3 &&
        !library_state_typed_values_scans);
    /* Neither declared output is opened until both real checked writers succeed. */
    library_state_typed_values_emit_packet(&source,source_path);
    library_state_typed_values_emit_packet(&linked,catalog_path);
    xr_xir_compile_checked_packet_free(&source);xr_xir_compile_checked_packet_free(&linked);
    fprintf(stderr,"typed-generation checked directSourceCalls=%zu (positive3 negative12 distinctFixtures10) directCheckedWrites=%zu CatalogBuilds=%zu explicitVerify=%zu fullScans=%zu fullFI=NOT_RUN\n",
        library_state_typed_values_source_calls,library_state_typed_values_checked_writes,
        library_state_typed_values_catalog_builds,library_state_typed_values_verifications,library_state_typed_values_scans);
}
int main(int argc,char **argv) {
    bool generating=argc==4 && !strcmp(argv[1],"--write-checked");
    CHECK(argc==1 || argc==3 || generating);
    if (argc==3) CHECK(strncmp(argv[1],"--",2));
    if (generating) CHECK(argv[2][0] && argv[3][0] && strcmp(argv[2],argv[3]));
    LibraryCompileOwner compiler={0};
    CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    if (generating) {
        library_state_typed_values_write_checked(&compiler.context,argv[2],argv[3]);
        XrCompileResourceStats measured=library_compile_stats(&compiler.context);
        fprintf(stderr,"typed-generation resources producer=checked allocated=%llu peak=%llu work=%llu\n",
            (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
        library_compile_owner_drop(&compiler);library_compile_observer_free();return 0;
    }
    library_compile_operation_cases("State selected slot map/group and scratch",library_state_typed_values_map_operation,NULL);
    library_compile_operation_cases("State canonical constructed slot type publication",library_state_typed_values_type_operation,NULL);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    LibrarySourceFixture source={NULL,0,XR_SOURCE_FIXTURES "/root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("State original whole Source",library_source_operation,&source);
    library_state_typed_values_program(&compiler.context,NULL,argc==3 ? argv[1] : NULL);
    XrXirArtifact *producer=library_state_typed_values_check(&compiler.context,XR_SOURCE_FIXTURES "/state.xr",XR_XIR_LIBRARY,NULL);
    library_state_typed_values_bad_slots(&compiler.context,producer);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(producer);
    char logical_path[16]="state.xr";
    XrXirLibraryModuleInput binding={.logical_path=logical_path,.authority=authority};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
    xr_sha256(input.packet,input.length,input.sha256);
    LibrarySourceFixture linked={&input,1,XR_SOURCE_FIXTURES "/root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("State Checked Catalog",library_catalog_operation,&linked);
    library_compile_operation_cases("State Catalog whole Source",library_source_operation,&linked);
    XrXirLibraryCatalog *catalog=NULL;CHECK(xr_xir_compile_library_catalog_new_v2(&compiler.context,&input,1,&catalog)==XR_XIR_OK);
    memset(packet.bytes,0,packet.length);xr_xir_compile_checked_packet_free(&packet);
    memset(logical_path,0,sizeof(logical_path));memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_state_typed_values_program(&compiler.context,catalog,argc==3 ? argv[2] : NULL);
    library_compile_owner_drop(&compiler);library_compile_observer_free();
    puts("Library private ordinary typed state Source/Catalog producer destruction PASS");return 0;
}
