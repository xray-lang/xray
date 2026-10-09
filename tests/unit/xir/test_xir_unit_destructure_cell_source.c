/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_unit_destructure_cell_source.c - Source and Checked Library private state ownership
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
static size_t unit_destructure_cell_source_calls,unit_destructure_cell_checked_writes;
static size_t unit_destructure_cell_catalog_builds,unit_destructure_cell_scans,unit_destructure_cell_verifications;
static void unit_destructure_cell_observe_cases(const char *name,LibraryCompileOperation operation,void *fixture) {
    ++unit_destructure_cell_scans;library_compile_operation_cases(name,operation,fixture);
}
static XrXirStatus unit_destructure_cell_observe_source(const XrXirSourceRequest *request,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic,char **failure_path) {
    ++unit_destructure_cell_source_calls;
    return xr_xir_compile_source_check(request,output,diagnostic,failure_path);
}
static XrXirStatus unit_destructure_cell_observe_write(const XrXirArtifact *artifact,
    XrXirCheckedPacket *output,XrXirDiagnostic *diagnostic) {
    ++unit_destructure_cell_checked_writes;
    return xr_xir_compile_checked_write(artifact,output,diagnostic);
}
static XrXirStatus unit_destructure_cell_observe_catalog(const XrXirCompileContext *context,
    const XrXirLibraryInput *inputs,size_t count,XrXirLibraryCatalog **output) {
    ++unit_destructure_cell_catalog_builds;
    return xr_xir_compile_library_catalog_new_v2(context,inputs,count,output);
}
static XrXirStatus unit_destructure_cell_observe_verify(const XrXirArtifact *artifact,XrXirDiagnostic *diagnostic) {
    ++unit_destructure_cell_verifications;return xr_xir_compile_artifact_verify(artifact,diagnostic);
}
/* These test counters observe real API calls after their implementations are
 * defined. They change no Source or Checked admission path. */
#define xr_xir_compile_source_check unit_destructure_cell_observe_source
#define xr_xir_compile_checked_write unit_destructure_cell_observe_write
#define xr_xir_compile_library_catalog_new_v2 unit_destructure_cell_observe_catalog
#define xr_xir_compile_artifact_verify unit_destructure_cell_observe_verify
#define library_compile_operation_cases unit_destructure_cell_observe_cases
#include "xir_library_source_owner.h"
#include "xir_unit_destructure_cell_runtime.h"
#include "xir_unit_destructure_cell_source_cases.h"

static void unit_destructure_cell_query(const XrXirSourceView *view,XrXirLinkageKind kind) {
    uint32_t units=0,bools=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *decl=&view->declarations[d];
        if (decl->kind!=XR_XIR_SOURCE_BINDING || !decl->name || !decl->mutable) continue;
        bool unit=!strcmp(decl->name,kind==XR_XIR_LIBRARY ? "stamp" : "ownStamp");
        bool boolean=!strcmp(decl->name,kind==XR_XIR_LIBRARY ? "flag" : "own");
        if (!unit && !boolean) continue;
        CHECK(decl->type.known && decl->type.type==(unit ? XR_XIR_UNIT : XR_XIR_BOOL));
        units+=unit;bools+=boolean;
    }
    CHECK(units==1 && bools==1);
}
static XrXirArtifact *unit_destructure_cell_check(const XrXirCompileContext *context,const char *path,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,bool negatives) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    if (negatives && kind==XR_XIR_PROGRAM) unit_destructure_cell_source_negatives(&request);
    else if (negatives) {
        unit_destructure_cell_negative(&request,"export_var.xr",XR_XIR_LIBRARY);
        unit_destructure_cell_negative(&request,"export_destructure.xr",XR_XIR_LIBRARY);
        unit_destructure_cell_negative(&request,"non_sendable.xr",XR_XIR_LIBRARY);
        unit_destructure_cell_negative(&request,"const_write.xr",XR_XIR_LIBRARY);
    }
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"state source %s status%u reason%s\n",path,status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view && view->complete);
    unit_destructure_cell_query(view,kind);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);return owned;
}
static void unit_destructure_cell_emit_packet(const XrXirCheckedPacket *packet,const char *path) {
    if (!path) return;
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(packet->bytes,1,packet->length,file)==packet->length && !fclose(file));
}
static void unit_destructure_cell_inventory(const XrXirArtifact *owned) {
    const XrXirModule *module=xr_xir_compile_artifact_module(owned);
    CHECK(module->declarations->slot_count==4);
    uint32_t counts[2][2]={{0}};
    for (uint32_t s=0;s<module->declarations->slot_count;++s) {
        const XrXirSlot *slot=&module->declarations->slots[s];
        CHECK(slot->mutable && xr_xir_type_is_cell(module->types,slot->type));
        XrXirType logical=xr_xir_cell_element(module->types,slot->type);
        CHECK(logical==XR_XIR_UNIT || logical==XR_XIR_BOOL);
        ++counts[slot->module!=module->declarations->root_module][logical==XR_XIR_BOOL];
    }
    CHECK(counts[0][0]==1 && counts[0][1]==1 && counts[1][0]==1 && counts[1][1]==1);
    uint32_t unit_cells=0,unit_writes=0,groups=0;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        for (uint32_t i=0;i<fn->instruction_count;++i) {
            const XrXirInstruction *op=&fn->instructions[i];
            if (op->op==XR_XIR_CELL_NEW && xr_xir_cell_is_unit(module->types,op->type)) {
                CHECK(op->args[0]==0 && xr_xir_cell_payload_operands(module->types,op->type,op->op)==0);
                ++unit_cells;
            }
            if (op->op==XR_XIR_CELL_WRITE) {
                uint32_t value=op->args[0];
                XrXirType cell=value<fn->parameter_count ? fn->parameters[value] :
                    fn->instructions[value-fn->parameter_count].type;
                if (xr_xir_cell_is_unit(module->types,cell)) {
                    CHECK(op->args[1]==0 && xr_xir_cell_payload_operands(module->types,cell,op->op)==1);
                    ++unit_writes;
                }
            }
            if (op->op==XR_XIR_SLOT_GROUP_INIT) {
                CHECK((uint32_t)op->immediate==2 && op->args[1]==2);++groups;
            }
        }
    }
    CHECK(unit_cells>=5 && unit_writes && groups==2);
}
static void unit_destructure_cell_program(const XrXirCompileContext *context,XrXirLibraryCatalog *catalog,const char *path,bool pending) {
    XrXirArtifact *owned=unit_destructure_cell_check(context,pending ? XR_SOURCE_FIXTURES "/pending.xr" : XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,catalog,!pending);
    unit_destructure_cell_inventory(owned);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    LibraryPacketFixture replay={packet.bytes,packet.length,NULL};
    library_compile_operation_cases(catalog ? "State Catalog whole Checked reader" : "State Source whole Checked reader",library_packet_operation,&replay);
    unit_destructure_cell_emit_packet(&packet,path);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_library_catalog_free(catalog);
    xr_test_unit_destructure_cell_run(context,owned,pending);
}
/* Build outputs use the same normal Source and Checked owners. Full fault
 * replay belongs to the separate test entry, so generation never invokes it. */
static void unit_destructure_cell_write_checked(const XrXirCompileContext *context,
    const char *source_path,const char *catalog_path,const char *pending_path) {
    XrXirCheckedPacket source={0},library={0},linked={0},pending={0};
    XrXirArtifact *owned=unit_destructure_cell_check(context,XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,NULL,true);
    unit_destructure_cell_inventory(owned);
    CHECK(xr_xir_compile_checked_write(owned,&source,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    owned=unit_destructure_cell_check(context,XR_SOURCE_FIXTURES "/state.xr",XR_XIR_LIBRARY,NULL,true);
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
    owned=unit_destructure_cell_check(context,XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,catalog,true);
    unit_destructure_cell_inventory(owned);xr_xir_compile_library_catalog_free(catalog);
    CHECK(xr_xir_compile_checked_write(owned,&linked,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    owned=unit_destructure_cell_check(context,XR_SOURCE_FIXTURES "/pending.xr",XR_XIR_PROGRAM,NULL,false);
    unit_destructure_cell_inventory(owned);
    CHECK(xr_xir_compile_checked_write(owned,&pending,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
    CHECK(unit_destructure_cell_source_calls==22 && unit_destructure_cell_checked_writes==4 &&
        unit_destructure_cell_catalog_builds==1 && unit_destructure_cell_verifications==4 &&
        !unit_destructure_cell_scans);
    /* All three declared outputs wait until their real checked writers succeed. */
    unit_destructure_cell_emit_packet(&source,source_path);
    unit_destructure_cell_emit_packet(&linked,catalog_path);
    unit_destructure_cell_emit_packet(&pending,pending_path);
    xr_xir_compile_checked_packet_free(&source);xr_xir_compile_checked_packet_free(&linked);
    xr_xir_compile_checked_packet_free(&pending);
    fprintf(stderr,"unit-destructure-generation checked directSourceCalls=%zu (positive4 negative18 distinctFixtures14) directCheckedWrites=%zu CatalogBuilds=%zu explicitVerify=%zu fullScans=%zu fullFI=NOT_RUN\n",
        unit_destructure_cell_source_calls,unit_destructure_cell_checked_writes,
        unit_destructure_cell_catalog_builds,unit_destructure_cell_verifications,unit_destructure_cell_scans);
}
int main(int argc,char **argv) {
    bool generating=argc==5 && !strcmp(argv[1],"--write-checked");
    CHECK(argc==1 || generating);
    if (generating) CHECK(argv[2][0] && argv[3][0] && argv[4][0] && strcmp(argv[2],argv[3]) &&
        strcmp(argv[2],argv[4]) && strcmp(argv[3],argv[4]));
    LibraryCompileOwner compiler={0};
    CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    if (generating) {
        unit_destructure_cell_write_checked(&compiler.context,argv[2],argv[3],argv[4]);
        XrCompileResourceStats measured=library_compile_stats(&compiler.context);
        fprintf(stderr,"unit-destructure-generation resources producer=checked allocated=%llu peak=%llu work=%llu\n",
            (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
        library_compile_owner_drop(&compiler);library_compile_observer_free();return 0;
    }
    library_compile_operation_cases("State selected slot map/group and scratch",unit_destructure_cell_map_operation,NULL);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    LibrarySourceFixture source={NULL,0,XR_SOURCE_FIXTURES "/root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("State original whole Source",library_source_operation,&source);
    unit_destructure_cell_program(&compiler.context,NULL,NULL,false);
    LibrarySourceFixture pending={NULL,0,XR_SOURCE_FIXTURES "/pending.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Unpublished Unit whole Source",library_source_operation,&pending);
    unit_destructure_cell_program(&compiler.context,NULL,NULL,true);
    XrXirArtifact *producer=unit_destructure_cell_check(&compiler.context,XR_SOURCE_FIXTURES "/state.xr",XR_XIR_LIBRARY,NULL,true);
    unit_destructure_cell_bad_slots(&compiler.context,producer);
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
    unit_destructure_cell_program(&compiler.context,catalog,NULL,false);
    library_compile_owner_drop(&compiler);library_compile_observer_free();
    puts("Mutable Unit destructuring real Source/Catalog and unpublished group ownership PASS");return 0;
}
