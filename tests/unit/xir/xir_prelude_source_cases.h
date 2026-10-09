/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Independent canonical inventory and actual same-ledger failure replays. */
#ifndef XIR_PRELUDE_SOURCE_CASES_H
#define XIR_PRELUDE_SOURCE_CASES_H
static XrXirStatus prelude_roundtrip(const XrXirCompileContext *context,
    const XrXirArtifact *artifact,XrXirArtifact **output) {
    XrXirCheckedPacket packets[2]={{0}}; XrXirArtifact *reads[2]={0};
    XrXirStatus status=xr_xir_compile_checked_write(artifact,&packets[0],NULL);
    if (status!=XR_XIR_OK) goto done;
    status=xr_xir_compile_checked_read(context,packets[0].bytes,packets[0].length,&reads[0],NULL);
    if (status!=XR_XIR_OK) goto done;
    CHECK(xr_xir_compile_artifact_context(reads[0])->resources==context->resources);
    status=xr_xir_compile_checked_write(reads[0],&packets[1],NULL);
    if (status!=XR_XIR_OK) goto done;
    CHECK(packets[0].length==packets[1].length && !memcmp(packets[0].bytes,packets[1].bytes,packets[0].length));
    status=xr_xir_compile_checked_read(context,packets[1].bytes,packets[1].length,&reads[1],NULL);
    if (status!=XR_XIR_OK) goto done;
    CHECK(xr_xir_compile_artifact_context(reads[1])->resources==context->resources);
    status=xr_xir_compile_artifact_verify(reads[1],NULL);
    if (status==XR_XIR_OK) { *output=reads[1]; reads[1]=NULL; }
done:
    for (unsigned i=0;i<2;++i) { xr_xir_compile_artifact_free(reads[i]);xr_xir_compile_checked_packet_free(&packets[i]); }
    return status;
}
static XrXirStatus prelude_factory_operation(const XrXirCompileContext *context,void *opaque) {
    (void)opaque; XrXirSourceResult result={0}; XrXirArtifact *read=NULL;
    XrXirStatus status=xr_xir_compile_prelude_library(context,&result,NULL);
    if (status==XR_XIR_OK) status=prelude_roundtrip(context,result.checked,&read);
    else CHECK(!result.checked && !result.snapshot);
    xr_xir_compile_artifact_free(read);xr_xir_compile_source_result_free(&result); return status;
}
static XrXirStatus prelude_source_operation(const XrXirCompileContext *context,void *opaque) {
    const LibrarySourceFixture *fixture=opaque; XrXirLibraryCatalog *catalog=NULL;
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirArtifact *read=NULL;
    XrXirStatus status=XR_XIR_OK;
    if (fixture->count) {
        status=xr_xir_compile_library_catalog_new_v2(context,fixture->inputs,fixture->count,&catalog);
        if (status!=XR_XIR_OK) goto done;
    }
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) {
        status=created==XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY; goto done;
    }
    XrXirSourceRequest request={session,fixture->entry,&fixture->authority,context,NULL,NULL,fixture->kind,catalog};
    status=xr_xir_compile_source_check(&request,&result,NULL,NULL);
    if (status==XR_XIR_OK) status=prelude_roundtrip(context,result.checked,&read);
    else CHECK(!result.checked && !result.snapshot);
    if (status==XR_XIR_OK) status=xr_test_prelude_compile_pipeline(context,read);
done:
    xr_xir_compile_artifact_free(read);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return status;
}
static void prelude_occupied(const XrXirCompileContext *context,XrXirSourceResult *result,
    const XrXirLibraryInput *input,XrXirLibraryCatalog *catalog) {
    XrCompileResourceStats before=library_compile_stats(context);size_t attempts=source_program_compile_attempts;
    XrXirSourceResult occupied=*result;
    CHECK(xr_xir_compile_prelude_library(context,&occupied,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(occupied.checked==result->checked && occupied.snapshot==result->snapshot);
    XrXirLibraryCatalog *held=catalog;
    CHECK(xr_xir_compile_library_catalog_new_v2(context,input,1,&held)==XR_XIR_BAD_STRUCTURE && held==catalog);
    XrCompileResourceStats after=library_compile_stats(context);
    CHECK(!memcmp(&before,&after,sizeof(before)) && attempts==source_program_compile_attempts);
}
static void prelude_catalog_negatives(const XrXirCompileContext *context,
    const XrXirArtifact *producer,XrXirLibraryModuleInput *binding) {
    const XrXirModule *original=xr_xir_compile_artifact_module(producer);
    CHECK(original->types->nominals->count==2);
    uint32_t crypto=UINT32_MAX,ordering=UINT32_MAX;
    for (uint32_t n=0;n<2;++n) {
        XrXirLiteral name=original->types->nominals->declarations[n].name;
        if (name.length==11 && !memcmp(name.bytes,"CryptoError",11)) crypto=n;
        if (name.length==8 && !memcmp(name.bytes,"Ordering",8)) ordering=n;
    }
    CHECK(crypto!=UINT32_MAX && ordering!=UINT32_MAX);
    for (unsigned mode=0;mode<4;++mode) {
        XrXirNominalDeclaration records[2];memcpy(records,original->types->nominals->declarations,sizeof(records));
        XrXirNominalVariant wrong={ {"WrongLength",11},0,0 };
        if (mode==0) records[crypto].name=(XrXirLiteral){"OtherError",10};
        if (mode==1) records[crypto].variants=&wrong;
        if (mode==2) {records[crypto].native=records[ordering].native;}
        if (mode==3) {records[ordering].native=(XrXirNominalNativeRecord){0};}
        XrXirNominalTable table=*original->types->nominals;table.declarations=records;
        XrXirTypes types=*original->types;types.nominals=&table;
        XrXirModule altered=*original;altered.types=&types;
        XrXirArtifact *owned=NULL;XrXirStatus status=xr_xir_compile_recheck_v2(context,&altered,
            xr_xir_compile_artifact_construction(producer),&owned,NULL);
        if (mode==2) {CHECK(status==XR_XIR_BAD_STRUCTURE && !owned);continue;}
        CHECK(status==XR_XIR_OK && owned); /* Ordinary malformed inventory is common-valid. */
        XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(owned,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(owned);
        XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=binding,.module_count=1};
        xr_sha256(input.packet,input.length,input.sha256);
        XrXirLibraryCatalog *output=NULL;
        CHECK(xr_xir_compile_library_catalog_new_v2(context,&input,1,&output)==XR_XIR_BAD_STRUCTURE && !output);
        xr_xir_compile_checked_packet_free(&packet);
    }
}
static void prelude_catalog_source_no_fallback(const XrXirCompileContext *context,XrXirLibraryCatalog *catalog) {
    size_t count=0;const XrModuleResourceBinding *resources=xr_xir_compile_library_catalog_resources_v2(catalog,&count);
    CHECK(count==1 && resources[0].checked);
    const XrXirModule *owned=xr_xir_compile_artifact_module(resources[0].checked);char *name=NULL;
    for (uint32_t n=0;n<owned->types->nominals->count;++n) {
        XrXirLiteral actual=owned->types->nominals->declarations[n].name;
        if (actual.length==11 && !memcmp(actual.bytes,"CryptoError",11)) name=(char *)actual.bytes;
    }
    CHECK(name);
    /* Deliberately poison the actual Catalog-owned ordinary declaration. This
     * adversarial boundary fixture is not a replacement Catalog or Source shim. */
    name[0]='X';XrCompileResourceStats before=library_compile_stats(context);
    for (unsigned repeat=0;repeat<2;++repeat) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
        XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/original_array_error_invoke_root.xr",&authority,
            context,NULL,NULL,XR_XIR_PROGRAM,catalog};XrXirSourceResult result={0};
        CHECK(xr_xir_compile_source_check(&request,&result,NULL,NULL)==XR_XIR_BAD_STRUCTURE && !result.checked && !result.snapshot);
        xr_compile_session_free(session);XrCompileResourceStats after=library_compile_stats(context);
        CHECK(after.allocated_bytes>=before.allocated_bytes && after.work>before.work && after.live_bytes==before.live_bytes);
        before=after;
    }
    name[0]='C';
    CHECK(xr_xir_compile_artifact_verify(resources[0].checked,NULL)==XR_XIR_OK);
}
#endif
