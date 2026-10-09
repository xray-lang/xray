/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_class_owned_cases.h - Owned construction verification data
 *
 * KEY CONCEPT:
 *   Consumer checks retain independent expected values, ownership and refusal duties.
 */
#ifndef XIR_LIBRARY_CLASS_OWNED_CASES_H
#define XIR_LIBRARY_CLASS_OWNED_CASES_H
#include "xir_library_typed_programs.h"
static void library_class_owned_case(void) {
    conflict_text("class_owned.xr",library_class_owned_definition);
    conflict_text("class_owned_receiver.xr",library_class_owned_consumer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"class_owned.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/class_owned.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(input.packet,input.length,input.sha256);
    CHECK(!remove(XR_SOURCE_FIXTURES "/class_owned.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/class_owned_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Owned library class Source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_typed_closed_negative(catalog,authority,"import \"./class_owned\" as lib;export fn result()->i64{return lib.Secret(41).read();}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./class_owned\" as lib;export fn result()->i64{return lib.make<i64>(41).value;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./class_owned\" as lib;export fn result()->i64{const box=lib.make<i64>(41);box.text=\"wrong\";return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./class_owned\" as lib;export fn result()->i64{var box:lib.Defaulted;return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./class_owned\" as lib;export fn result()->i64{const box=lib.Box<i64>{value:41,text:\"owned\"};return 41;}",XR_XIR_BAD_TYPE);
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/class_owned_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"library class %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    uint32_t classes=0;
    for(uint32_t n=0;n<module->types->nominals->count;++n)
        classes+=module->types->nominals->declarations[n].kind==XR_XIR_NOMINAL_CLASS;
    CHECK(classes==3&&module->declarations->implementations&&module->declarations->implementations->count==1);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/class_owned_receiver.xr"));
    puts("class literal41 owned5 alias identity explicit private implicit construction generic witness producerdeath twoInstances physical0 PASS");
}
#endif // XIR_LIBRARY_CLASS_OWNED_CASES_H
