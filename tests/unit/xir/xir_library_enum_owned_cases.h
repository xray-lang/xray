/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_enum_owned_cases.h - Owned construction verification data
 *
 * KEY CONCEPT:
 *   Consumer checks retain independent expected values, ownership and refusal duties.
 */
#ifndef XIR_LIBRARY_ENUM_OWNED_CASES_H
#define XIR_LIBRARY_ENUM_OWNED_CASES_H
#include "xir_library_typed_programs.h"
static void library_enum_owned_case(void) {
    conflict_text("enum_owned.xr",library_enum_owned_definition);
    conflict_text("enum_owned_receiver.xr",library_enum_owned_consumer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"enum_owned.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/enum_owned.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(input.packet,input.length,input.sha256);
    CHECK(!remove(XR_SOURCE_FIXTURES "/enum_owned.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/enum_owned_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Owned library enum Source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_typed_closed_negative(catalog,authority,"import {Hidden} from \"./enum_owned\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
    library_typed_closed_negative(catalog,authority,"import {item} from \"./enum_owned\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
    library_typed_closed_negative(catalog,authority,"import \"./enum_owned\" as lib;export fn result()->i64{const x=lib.Packet<i64>.Value{item:41};return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./enum_owned\" as lib;export fn result()->i64{const x=lib.Packet<i64>.Value{item:\"bad\",text:\"owned\"};return 41;}",XR_XIR_BAD_TYPE);
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/enum_owned_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"library enum %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    CHECK(module->types&&module->types->nominals&&module->types->nominals->count==3);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/enum_owned_receiver.xr"));
    puts("enum literal41 owned5 ordinal1 Value Packet.Value generic member named payload producerdeath twoInstances physical0 PASS");
}
#endif // XIR_LIBRARY_ENUM_OWNED_CASES_H
