/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_typed_closed_cases.h - Owned construction verification data
 *
 * KEY CONCEPT:
 *   Consumer checks retain independent expected values, ownership and refusal duties.
 */
/* Real Source -> owned Checked Library -> AST-free receiver. Fixed results are
 * authored here before execution; no census result defines semantic truth. */
#ifndef XIR_LIBRARY_TYPED_CLOSED_CASES_H
#define XIR_LIBRARY_TYPED_CLOSED_CASES_H
#include "xir_library_typed_programs.h"

static void library_typed_closed_negative(XrXirLibraryCatalog *catalog,
    XrModuleIdentityAuthority authority,const char *text,XrXirStatus expected) {
    conflict_text("typed_reject.xr",text);
    size_t blocks=source_live,bytes=source_bytes,rblocks=runtime_live,rbytes=runtime_bytes;
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/typed_reject.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=expected)fprintf(stderr,"typed negative expected=%u actual=%u %s\n",expected,status,diagnostic.message);
    CHECK(status==expected&&!result.checked&&!result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    CHECK(source_live==blocks&&source_bytes==bytes&&runtime_live==rblocks&&runtime_bytes==rbytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/typed_reject.xr"));
}
static void library_typed_closed_case(unsigned mode) {
    CHECK(mode<2);
    conflict_text("typed_closed.xr",library_typed_definition);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    const char *names[]={"typed_closed.xr"};XrXirLibraryModuleInput binding={0};size_t count=0;
    XrXirCheckedPacket packet=library_views_packet(XR_SOURCE_FIXTURES "/typed_closed.xr",authority,names,1,&binding,&count);
    CHECK(count==1);
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(input.packet,input.length,input.sha256);
    conflict_text("typed_receiver.xr",library_typed_consumers[mode]);
    CHECK(!remove(XR_SOURCE_FIXTURES "/typed_closed.xr"));
    LibrarySourceFixture fixture={&input,1,XR_SOURCE_FIXTURES "/typed_receiver.xr",authority,XR_XIR_PROGRAM};
    library_compile_operation_cases(mode?"Typed interface witness source":"Typed nominal source",library_source_operation,&fixture);
    XrXirLibraryCatalog *catalog=NULL;
    CHECK(xr_xir_compile_library_catalog_new_v2(library_context,&input,1,&catalog)==XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);memset(&input,0,sizeof(input));memset(&binding,0,sizeof(binding));
    /* Private declarations and interface members never become top-level exports. */
    if(!mode){
        library_typed_closed_negative(catalog,authority,"import {Secret} from \"./typed_closed\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
        library_typed_closed_negative(catalog,authority,"import {Hidden} from \"./typed_closed\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
        library_typed_closed_negative(catalog,authority,"import {measure} from \"./typed_closed\";export fn result()->i64{return 41;}",XR_XIR_BAD_STRUCTURE);
        library_typed_closed_negative(catalog,authority,"import {secret} from \"./typed_closed\";export fn result()->i64{return secret();}",XR_XIR_BAD_STRUCTURE);
        library_typed_closed_negative(catalog,authority,"import \"./typed_closed\" as lib;export fn result()->i64{const value=lib.Locked{text:\"hidden\"};return 41;}",XR_XIR_BAD_TYPE);
        library_typed_closed_negative(catalog,authority,"import \"./typed_closed\" as lib;export fn result()->i64{const value=lib.Payload{n:41};return 41;}",XR_XIR_BAD_TYPE);
        library_typed_closed_negative(catalog,authority,"import \"./typed_closed\" as lib;export fn result()->i64{const value=lib.Payload();return 41;}",XR_XIR_BAD_TYPE);
    }
    XrCompilerSession *session=library_session_new(library_context);
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/typed_receiver.xr",&authority,library_context,NULL,NULL,XR_XIR_PROGRAM,catalog};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"typed positive mode%u %u %s\n",mode,status,diagnostic.message);
    CHECK(status==XR_XIR_OK&&result.checked&&result.snapshot);
    const XrXirModule *checked=xr_xir_compile_artifact_module(result.checked);
    CHECK(checked->types&&checked->types->nominals&&checked->types->nominals->count==4);
    CHECK(checked->types->interfaces&&checked->types->interfaces->count==2);
    CHECK(checked->declarations->module_count==2);
    /* 3 library nominal declarations + 1 receiver nominal; 2 library interfaces.
     * A receiver-local type precedes or follows imported types independently of
     * packet ordinals. Common verification must resolve each canonical type. */
    uint32_t payload=UINT32_MAX,local=UINT32_MAX;
    for(uint32_t n=0;n<checked->types->nominals->count;++n){
        const XrXirNominalDeclaration *d=&checked->types->nominals->declarations[n];
        if(d->name.length==7&&!memcmp(d->name.bytes,"Payload",7)){CHECK(payload==UINT32_MAX);payload=n;CHECK(d->exported&&d->field_count==2&&d->fields[0].type==XR_XIR_STRING&&d->fields[1].type==XR_XIR_I64);}
        if(d->name.length==5&&!memcmp(d->name.bytes,"Local",5)){CHECK(local==UINT32_MAX);local=n;}
    }
    CHECK(payload!=UINT32_MAX&&local!=UINT32_MAX&&payload!=local);
    CHECK(mode?checked->declarations->implementations&&checked->declarations->implementations->count==1:
        !checked->declarations->implementations);
    XrXirArtifact *owned=result.checked;result.checked=NULL;
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_test_library_source_run(owned);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!remove(XR_SOURCE_FIXTURES "/typed_receiver.xr"));
    printf("typed closed mode%u literal41 bytes typed/local realSource producerdeath twoInstances physical0 PASS\n",mode);
}
#endif // XIR_LIBRARY_TYPED_CLOSED_CASES_H
