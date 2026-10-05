/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_source_owner.h - Same-ledger original Source replay fixture
 */
#ifndef XIR_LIBRARY_SOURCE_OWNER_H
#define XIR_LIBRARY_SOURCE_OWNER_H
typedef struct LibrarySourceFixture {
    const XrXirLibraryInput *inputs;size_t count;
    const char *entry;XrModuleIdentityAuthority authority;
    XrXirLinkageKind kind;
} LibrarySourceFixture;
static XrXirStatus library_source_operation(const XrXirCompileContext *context,void *opaque) {
    const LibrarySourceFixture *fixture=opaque;XrXirLibraryCatalog *catalog=NULL;
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirCheckedPacket packet={0};
    XrXirStatus status=XR_XIR_OK;
    if(fixture->count){status=xr_xir_compile_library_catalog_new(context,fixture->inputs,fixture->count,&catalog);if(status!=XR_XIR_OK){CHECK(!catalog);goto done;}}
    XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if(created!=XR_COMPILER_SESSION_OK){CHECK(!session);status=created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    XrXirSourceRequest request={session,fixture->entry,&fixture->authority,context,NULL,NULL,fixture->kind,catalog};
    status=xr_xir_compile_source_check(&request,&result,NULL,NULL);
    if(status!=XR_XIR_OK){CHECK(!result.checked&&!result.snapshot);goto done;}
    CHECK(result.checked&&result.snapshot&&xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
    status=xr_xir_compile_checked_write(result.checked,&packet,NULL);
    if(status!=XR_XIR_OK)CHECK(!packet.bytes&&!packet.length);
 done:
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);return status;
}
static XrXirStatus library_catalog_operation(const XrXirCompileContext *context,void *opaque) {
    const LibrarySourceFixture *fixture=opaque;XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new(context,fixture->inputs,fixture->count,&catalog);
    CHECK(status==XR_XIR_OK?catalog!=NULL:catalog==NULL);xr_xir_compile_library_catalog_free(catalog);return status;
}
typedef struct LibraryPacketFixture {const void *bytes;size_t length;const XrXirModule *module;} LibraryPacketFixture;
static inline XrXirStatus library_packet_operation(const XrXirCompileContext *context,void *opaque) {
    const LibraryPacketFixture *fixture=opaque;XrXirArtifact *artifact=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=fixture->module?xr_xir_compile_recheck(context,fixture->module,&artifact,NULL):xr_xir_compile_checked_read(context,fixture->bytes,fixture->length,&artifact,NULL);
    if(status!=XR_XIR_OK){CHECK(!artifact);return status;}
    if(fixture->bytes){status=xr_xir_compile_checked_write(artifact,&packet,NULL);if(status==XR_XIR_OK)CHECK(packet.length==fixture->length&&!memcmp(packet.bytes,fixture->bytes,packet.length));else CHECK(!packet.bytes&&!packet.length);}
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(artifact);return status;
}
#endif
