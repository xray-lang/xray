/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_carrier_member_cases.h - Owned construction verification data
 *
 * KEY CONCEPT:
 *   Consumer checks retain independent expected values, ownership and refusal duties.
 */
#ifndef XIR_LIBRARY_CARRIER_MEMBER_CASES_H
#define XIR_LIBRARY_CARRIER_MEMBER_CASES_H
#include "xir_library_typed_programs.h"
/* The literals below are semantic oracles; no resource boundary observation
 * supplies an expected value or changes the finite compiler limits. */
static void library_carrier_member_case(void) {
    conflict_text("carrier_member.xr",library_carrier_member_definition);
    conflict_text("carrier_member_receiver.xr",library_carrier_member_consumer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"carrier_member.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/carrier_member.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};xr_sha256(input.packet,input.length,input.sha256);
    CHECK(!remove(XR_SOURCE_FIXTURES "/carrier_member.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/carrier_member_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases("Owned library carrier member Source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    library_typed_closed_negative(catalog,authority,"import {bump} from \"./carrier_member\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
    library_typed_closed_negative(catalog,authority,"import \"./carrier_member\" as lib;export fn result()->i64{const c=lib.Counter(40);c.bump();return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./carrier_member\" as lib;export fn result()->i64{const x=lib.Secret(\"hidden\");const text=x.text;return 41;}",XR_XIR_BAD_TYPE);
    library_typed_closed_negative(catalog,authority,"import \"./carrier_member\" as lib;export fn result()->i64{const x=lib.Envelope<i64>{entries:[(\"bad\",null)],text:\"owned\"};return 41;}",XR_XIR_BAD_TYPE);
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/carrier_member_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"library carrier member %u %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    bool array=false,tuple=false,nullable=false,cell=false,reference=false;
    for(uint32_t t=0;t<module->types->count;++t){
        XrXirTypeKind kind=module->types->nodes[t].kind;
        array|=kind==XR_XIR_TYPE_ARRAY;tuple|=kind==XR_XIR_TYPE_TUPLE;
        nullable|=kind==XR_XIR_TYPE_NULLABLE;cell|=kind==XR_XIR_TYPE_CELL;
    }
    for(uint32_t f=0;f<module->function_count;++f)
        for(uint32_t i=0;i<module->functions[f].instruction_count;++i)
            reference|=module->functions[f].instructions[i].op==XR_XIR_FUNCTION_REF;
    CHECK(array&&tuple&&nullable&&cell&&reference);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/carrier_member_receiver.xr"));
    puts("carrier member literal41 owned5 nested tuple nullable ref explicit constructor realSource producerdeath twoInstances physical0 PASS");
}
#endif // XIR_LIBRARY_CARRIER_MEMBER_CASES_H
