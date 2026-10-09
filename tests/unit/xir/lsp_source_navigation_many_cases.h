/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static XrXirStatus lsp_many_json_run(XrCompileResources *resources,XlspSourceSnapshot **snapshots,
    const char *root_uri,const char *library_uri) {
    XrJsonValue *output=NULL;
    XrXirStatus status=xlsp_source_navigation_many_json(resources,snapshots,2,library_uri,strlen(library_uri),
        (XrLspPosition){0,20},1,false,&output);
    if(status!=XR_XIR_OK){CHECK(!output);return status;}
    CHECK(output->as.array.count==1);
    lsp_json_location_facts(output->as.array.items[0],root_uri,1,42,47);xjson_free(output);output=NULL;
    status=xlsp_source_navigation_many_json(resources,snapshots,2,library_uri,strlen(library_uri),
        (XrLspPosition){0,20},1,true,&output);
    if(status!=XR_XIR_OK){CHECK(!output);return status;}
    CHECK(output->as.array.count==2);
    lsp_json_location_facts(output->as.array.items[0],library_uri,0,19,24);
    lsp_json_location_facts(output->as.array.items[1],root_uri,1,42,47);xjson_free(output);output=NULL;
    status=xlsp_source_navigation_many_json(resources,snapshots,2,library_uri,strlen(library_uri),
        (XrLspPosition){0,20},2,true,&output);
    if(status!=XR_XIR_OK){CHECK(!output);return status;}
    CHECK(output->as.array.count==1);
    XrJsonValue *highlight=output->as.array.items[0];CHECK(xjson_get_int(highlight,"kind")==3);
    XrJsonValue *range=xjson_get_object(highlight,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==19&&
        xjson_get_int(end,"line")==0&&xjson_get_int(end,"character")==24);
    xjson_free(output);return XR_XIR_OK;
}
static void lsp_navigation_many_cases(const char *root) {
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    char root_uri[XR_TEST_PATH_MAX],library_uri[XR_TEST_PATH_MAX];
    lsp_uri(root_uri,sizeof(root_uri),root,"root.xr");lsp_uri(library_uri,sizeof(library_uri),root,"lib.xr");
    XlspSourceDocument documents[2]={
        {root_uri,source_root_text,strlen(root_uri),sizeof(source_root_text)-1,1},
        {library_uri,source_library_text,strlen(library_uri),sizeof(source_library_text)-1,7}};
    XrModuleIdentityAuthority authorities[2]={{XR_MODULE_IDENTITY_SCRIPT,NULL,root},{XR_MODULE_IDENTITY_SCRIPT,NULL,root}};
    XrXirSourceRequest request={session,NULL,&authorities[0],&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceSnapshot *snapshots[2]={0};
    CHECK(xlsp_source_snapshot_build_authorities(&request,documents,authorities,2,0,&snapshots[0],NULL,NULL)==XR_XIR_OK);
    CHECK(xlsp_source_snapshot_build_authorities(&request,documents,authorities,2,1,&snapshots[1],NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);xr_compile_resources_release(context.resources);
    /* Root closure has declaration+call, library closure only declaration.
     * Their union must keep call once, and declaration only when requested. */
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
        XrXirStatus status=lsp_many_json_run(context.resources,snapshots,root_uri,library_uri);
        if(!probe){CHECK(status==XR_XIR_OK);sites=lsp_json_calls;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected&&lsp_json_calls==probe);
        CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrJsonValue *output=NULL;
    CHECK(xlsp_source_navigation_many_json(context.resources,snapshots,2,root_uri,strlen(root_uri),
        (XrLspPosition){1,43},0,false,&output)==XR_XIR_OK);
    xlsp_source_snapshot_free(snapshots[1]);xlsp_source_snapshot_free(snapshots[0]);lsp_zero();
    lsp_json_location_facts(output,library_uri,0,19,24);xjson_free(output);
    CHECK(!lsp_json_live&&!lsp_json_bytes);
}
