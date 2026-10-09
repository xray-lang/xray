/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_generic_parent_cases.h - Owned construction verification data
 *
 * KEY CONCEPT:
 *   Consumer checks retain independent expected values, ownership and refusal duties.
 */
#ifndef XIR_LIBRARY_GENERIC_PARENT_CASES_H
#define XIR_LIBRARY_GENERIC_PARENT_CASES_H
#include "xir_library_typed_programs.h"
static void library_generic_parent_case(unsigned reverse) {
    CHECK(reverse<2);
    conflict_text("generic_parent.xr",library_generic_parent_definitions[reverse]);
    conflict_text("generic_parent_receiver.xr",library_generic_parent_consumer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"generic_parent.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/generic_parent.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(input.packet,input.length,input.sha256);
    CHECK(!remove(XR_SOURCE_FIXTURES "/generic_parent.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/generic_parent_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases(reverse?"Generic parents right-left Source":"Generic parents left-right Source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_typed_closed_negative(catalog,authority,"import {Private} from \"./generic_parent\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
    library_typed_closed_negative(catalog,authority,"import {measure} from \"./generic_parent\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
    library_typed_closed_negative(catalog,authority,"import \"./generic_parent\" as lib;export fn result()->i64{const value=lib.Box<bool>{value:true,text:\"owned\"};return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./generic_parent\" as lib;struct Bad implements lib.Diamond<i64>{measure(value:string)->i64{return 41;}}export fn result()->i64{return 41;}",XR_XIR_BAD_TYPE);
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/generic_parent_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"generic parent %u %u %s\n",reverse,status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    CHECK(module->types&&module->types->interfaces&&module->types->interfaces->count==7);
    CHECK(module->declarations->implementations&&module->declarations->implementations->count==2);
    /* Six imported interfaces + LocalFirst. Two independently local witnesses.
     * Neither interface ordinals nor canonical type IDs are assumed equal. */
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/generic_parent_receiver.xr"));
    printf("generic parent mode%u literal41 ownedbytes5 realSource producerdeath twoInstances physical0 PASS\n",reverse);
}
#endif // XIR_LIBRARY_GENERIC_PARENT_CASES_H
