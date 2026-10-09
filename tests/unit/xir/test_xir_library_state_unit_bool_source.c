/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_state_unit_bool_source.c - Source and Checked Library private state ownership
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
#include "xir_library_source_owner.h"
#include "xir_library_state_unit_bool_runtime.h"
#include "xir_library_state_unit_bool_source_cases.h"

static XrXirArtifact *library_state_unit_bool_check(const XrXirCompileContext *context,const char *path,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    if (kind==XR_XIR_PROGRAM) library_state_unit_bool_source_negatives(&request);
    else {
        library_state_unit_bool_negative(&request,"export_var.xr",XR_XIR_LIBRARY);
        library_state_unit_bool_negative(&request,"export_destructure.xr",XR_XIR_LIBRARY);
        library_state_unit_bool_negative(&request,"non_sendable.xr",XR_XIR_LIBRARY);
        library_state_unit_bool_negative(&request,"const_write.xr",XR_XIR_LIBRARY);
    }
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"state source %s status%u reason%s\n",path,status,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);CHECK(view && view->complete);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);return owned;
}
static void library_state_unit_bool_emit_packet(const XrXirCheckedPacket *packet,const char *path) {
    if (!path) return;
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(packet->bytes,1,packet->length,file)==packet->length && !fclose(file));
}
static void library_state_unit_bool_program(const XrXirCompileContext *context,XrXirLibraryCatalog *catalog,const char *path) {
    XrXirArtifact *owned=library_state_unit_bool_check(context,XR_SOURCE_FIXTURES "/root.xr",XR_XIR_PROGRAM,catalog);
    const XrXirModule *module=xr_xir_compile_artifact_module(owned);
    CHECK(module->declarations->slot_count==3);
    uint32_t library_slots=0,unit_slots=0,bool_slots=0;
    for (uint32_t s=0;s<module->declarations->slot_count;++s) {
        const XrXirSlot *slot=&module->declarations->slots[s];
        CHECK(slot->mutable);
        if (slot->module==module->declarations->root_module) {CHECK(slot->type==XR_XIR_BOOL);continue;}
        ++library_slots;unit_slots+=slot->type==XR_XIR_UNIT;bool_slots+=slot->type==XR_XIR_BOOL;
    }
    CHECK(library_slots==2 && unit_slots==1 && bool_slots==1);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
    LibraryPacketFixture replay={packet.bytes,packet.length,NULL};
    library_compile_operation_cases(catalog ? "State Catalog whole Checked reader" : "State Source whole Checked reader",library_packet_operation,&replay);
    library_state_unit_bool_emit_packet(&packet,path);xr_xir_compile_checked_packet_free(&packet);
    xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_state_unit_bool_run(context,owned);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==3);LibraryCompileOwner compiler={0};
    CHECK(library_compile_owner_new(&compiler,&library_compile_limits)==XR_XIR_OK);
    library_compile_operation_cases("State selected slot map/group and scratch",library_state_unit_bool_map_operation,NULL);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    LibrarySourceFixture source={NULL,0,XR_SOURCE_FIXTURES "/root.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("State original whole Source",library_source_operation,&source);
    library_state_unit_bool_program(&compiler.context,NULL,argc==3 ? argv[1] : NULL);
    XrXirArtifact *producer=library_state_unit_bool_check(&compiler.context,XR_SOURCE_FIXTURES "/state.xr",XR_XIR_LIBRARY,NULL);
    library_state_unit_bool_bad_slots(&compiler.context,producer);
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
    library_state_unit_bool_program(&compiler.context,catalog,argc==3 ? argv[2] : NULL);
    library_compile_owner_drop(&compiler);library_compile_observer_free();
    puts("Library private Unit/bool state Source/Catalog producer destruction PASS");return 0;
}
