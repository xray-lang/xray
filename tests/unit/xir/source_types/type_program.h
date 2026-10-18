/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static const XrCompileResourceLimits type_limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static XrXirStatus type_resource_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus type_manifest_status(XrManifestStatus status) {
    return status==XR_MANIFEST_OK?XR_XIR_OK:status==XR_MANIFEST_BUDGET?XR_XIR_BUDGET:
        status==XR_MANIFEST_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:status==XR_MANIFEST_IO?XR_XIR_IO:
        status==XR_MANIFEST_NOT_FOUND?XR_XIR_UNRESOLVED:status==XR_MANIFEST_UNSUPPORTED?XR_XIR_UNSUPPORTED:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus type_program(const char *path,unsigned mode,XrCompileResourceLimits bounds,
    XrCompileResourceStats *stats,XrXirSourceSnapshot **output,XrXirSourceDiagnostic *diagnostic) {
    CHECK(mode<5&&output&&!*output&&diagnostic);*diagnostic=(XrXirSourceDiagnostic){0};
    size_t length=strlen(texts[mode]);char *text=sdk_fixture_malloc(length+1);CHECK(text);memcpy(text,texts[mode],length+1);
    XrCompileResources *r=NULL;XrXirStatus status=type_resource_status(xr_compile_resources_new(&bounds,&r));
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};XrStdlibPath stdlib={0};
    XrXirLibraryCatalog *catalog=NULL;XrSourceAuthority *authority=NULL;XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    if(status!=XR_XIR_OK)goto done;
    XrSourceCompileStatus selected=xr_source_stdlib_path(r,&stdlib,NULL);
    if(selected!=XR_SOURCE_COMPILE_OK){status=selected==XR_SOURCE_COMPILE_BUDGET?XR_XIR_BUDGET:
        selected==XR_SOURCE_COMPILE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:selected==XR_SOURCE_COMPILE_NOT_FOUND?XR_XIR_UNRESOLVED:XR_XIR_IO;goto done;}
    status=xir_stdlib_library_catalog_new(&context,stdlib.path,&catalog);if(status!=XR_XIR_OK)goto done;
    XrSourceEntryInput input={XR_SOURCE_ENTRY_TEXT,path,text,length};XrTomlParseLimits toml=xr_source_compile_default_manifest_limits();
    status=type_manifest_status(xr_source_authority_open_input(&context,&input,catalog,&toml,&authority,NULL));if(status!=XR_XIR_OK)goto done;
    XrCompilerSessionStatus opened=xr_compile_session_new(r,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
        opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;}
    XrXirSourceRequest request={session,xr_source_authority_source_path(authority),xr_source_authority_entry(authority),&context,
        stdlib.path,xr_source_authority_lockfile(authority),XR_XIR_PROGRAM,catalog};
    status=xr_xir_compile_source_check_overlay(&request,xr_source_authority_overlay(authority),1,&result,diagnostic,NULL);
    if(status==XR_XIR_OK){*output=result.snapshot;result.snapshot=NULL;}
done:
    if(stats)*stats=r?sdk_stats(r):(XrCompileResourceStats){0};diagnostic->status=status;
    memset(text,'?',length);sdk_fixture_free(text);xr_xir_compile_source_result_free(&result);
    xr_source_authority_close(authority);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    xr_source_stdlib_path_free(&stdlib);xr_compile_resources_release(r);return status;
}
