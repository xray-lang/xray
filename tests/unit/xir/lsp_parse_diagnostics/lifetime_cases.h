/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_PARSE_FAILURE_LIFETIME_H
#define LSP_PARSE_FAILURE_LIFETIME_H
static void diagnostic_failure_lifetime(const DiagnosticCase *input) {
    XrCompileResources *resources=sdk_ledger(&input->limits);XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    char root[4096],library[4096],directory[4096];
    CHECK(strlen(input->root_path)<sizeof(root)&&strlen(input->lib_path)<sizeof(library)&&strlen(input->directory)<sizeof(directory));
    strcpy(root,input->root_path);strcpy(library,input->lib_path);strcpy(directory,input->directory);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,directory};
    XrModuleOverlayInput overlays[2]={{authority,"root.xr",root,root_source,sizeof(root_source)-1},
        {authority,"lib.xr",library,bad_source,sizeof(bad_source)-1}};
    XrXirSourceRequest request={session,root,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    CHECK(xr_xir_compile_source_check_overlay(&request,overlays,2,&result,&diagnostic,&failure)==XR_XIR_BAD_STRUCTURE);
    CHECK(!result.checked&&!result.snapshot&&failure.path&&failure.path!=library);
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    memset(root,'?',sizeof(root));memset(library,'?',sizeof(library));memset(directory,'?',sizeof(directory));
    memset(&request,0,sizeof(request));memset(overlays,0,sizeof(overlays));memset(&context,0,sizeof(context));
    CHECK(runtime_live&&runtime_bytes&&!strcmp(failure.path,input->lib_path));
    CHECK(failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE&&failure.has_utf16_position&&
        failure.utf16_line==0&&failure.utf16_column==13&&!strcmp(diagnostic.message,"expected variable name"));
    size_t attempts=runtime_attempts;runtime_fail_at=attempts;
    xr_xir_compile_source_failure_free(&failure);xr_xir_compile_source_failure_free(&failure);
    xr_xir_compile_source_failure_free(NULL);runtime_fail_at=SIZE_MAX;
    CHECK(xr_xir_compile_source_failure_empty(&failure)&&runtime_attempts==attempts&&!runtime_live&&!runtime_bytes);
    fprintf(stderr,"PARSE_FAILURE_LIFETIME producer_and_caller_ledger_dead=1 input_poisoned=1 no_cleanup_allocation=1 idempotent=1 physical0/0\n");
}
#endif // LSP_PARSE_FAILURE_LIFETIME_H
