/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_member_witness_cases.h - Owned construction verification data
 *
 * KEY CONCEPT:
 *   Consumer checks retain independent expected values, ownership and refusal duties.
 */
#ifndef XIR_LIBRARY_MEMBER_WITNESS_CASES_H
#define XIR_LIBRARY_MEMBER_WITNESS_CASES_H
#include "xir_library_typed_programs.h"
static void library_member_witness_attack(const XrXirArtifact *artifact) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(artifact);
    const XrXirImplementationTable *table=module->declarations->implementations;
    CHECK(table&&table->count==1&&table->records[0].binding_count==1);
    XrXirImplementation record=table->records[0];
    XrXirImplementationBinding binding=record.bindings[0];
    binding.function=module->declarations->modules[0].initializer;
    record.bindings=&binding;
    XrXirImplementationTable badtable={&record,1};
    XrXirDeclarations declarations=*module->declarations;declarations.implementations=&badtable;
    XrXirModule altered=*module;altered.declarations=&declarations;
    library_construction_reject(&altered,construction);
}
static void library_member_witness_case(void) {
    conflict_text("member_witness.xr",library_member_witness_definition);
    conflict_text("member_witness_receiver.xr",library_member_witness_consumer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"member_witness.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/member_witness.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirArtifact *decoded=NULL;
    CHECK(xr_xir_compile_checked_read(library_context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    library_member_witness_attack(decoded);
    xr_xir_compile_artifact_free(decoded);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(input.packet,input.length,input.sha256);
    CHECK(!remove(XR_SOURCE_FIXTURES "/member_witness.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/member_witness_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Owned library member witness Source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_typed_closed_negative(catalog,authority,"import {read} from \"./member_witness\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
    library_typed_closed_negative(catalog,authority,"import \"./member_witness\" as lib;export fn result()->i64{return lib.make().secret();}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./member_witness\" as lib;export fn result()->i64{return lib.make().echo<i64>(41);}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./member_witness\" as lib;export fn result()->i64{return lib.make().choose<i64,string>(41);}",XR_XIR_BAD_TYPE);
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/member_witness_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"library member witness %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    CHECK(module->types&&module->types->interfaces&&module->types->interfaces->count==3);
    library_member_witness_attack(result.checked);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/member_witness_receiver.xr"));
    puts("member witness literal41 owned5 member6 realSource producerdeath twoInstances physical0 PASS");
}
#endif // XIR_LIBRARY_MEMBER_WITNESS_CASES_H
