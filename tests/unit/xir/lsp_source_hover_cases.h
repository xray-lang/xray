/* Independent oracle: literal names/types, never refreshed from query output. */
static void lsp_hover_json_facts(XrJsonValue *output) {
    CHECK(output&&output->type==XR_JSON_OBJECT&&output->as.object.count==1);
    XrJsonValue *contents=xjson_get_object(output,"contents");CHECK(contents&&contents->as.object.count==2);
    CHECK(!strcmp(xjson_get_string(contents,"kind"),"plaintext"));
    CHECK(!strcmp(xjson_get_string(contents,"value"),"export fn value() -> i64"));
}
static XrXirStatus lsp_hover_typed_once(const char *root,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrXirCompileContext context={0};XrCompilerSession *session=NULL;XlspSourceSnapshot *snapshot=NULL;XlspSourceHover text={0};
    XrCompileResourceStatus resource=xr_compile_resources_new(&limits,&context.resources);
    if(resource!=XR_COMPILE_RESOURCE_OK)return resource==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    context.limits=xr_xir_compile_default_limits();
    XrCompilerSessionStatus made=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:made==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char uri[XR_TEST_PATH_MAX];lsp_uri(uri,sizeof(uri),root,"typed.xr");
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    /* Nine actual nested constructors cross the renderer's initial eight-frame
     * allocation. Long complete signature crosses both text growth boundaries. */
#define HOVER_SOURCE_TYPE "Array<Array<Array<Array<Array<Array<Array<Array<Array<string>>>>>>>>>"
#define HOVER_DISPLAY_TYPE "Array<Array<Array<Array<Array<Array<Array<Array<Array<String>>>>>>>>>"
    static const char typed[]="fn nested(rows:" HOVER_SOURCE_TYPE ")->" HOVER_SOURCE_TYPE " { return rows }\n"
        "fn generic<T>(x:T)->T { return x }\n"
        "fn apply(action:fn(ref i64)->i64,value:ref i64)->i64 { return action(ref value) }\n";
    XlspSourceDocument doc={uri,typed,strlen(uri),sizeof(typed)-1,9};
    if(status==XR_XIR_OK)status=xlsp_source_snapshot_build(&request,&doc,1,0,&snapshot,NULL,NULL);
    xr_compile_session_free(session);session=NULL;
    if(status==XR_XIR_OK)status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){0,4},&text);
    if(status!=XR_XIR_OK)goto done;
    CHECK(!strcmp(text.text,"fn nested(rows: " HOVER_DISPLAY_TYPE ") -> " HOVER_DISPLAY_TYPE));xlsp_source_hover_free(&text);
    status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){0,10},&text);if(status!=XR_XIR_OK)goto done;
    CHECK(!strcmp(text.text,"parameter rows: " HOVER_DISPLAY_TYPE));xlsp_source_hover_free(&text);
    status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){1,4},&text);
    if(status==XR_XIR_UNSUPPORTED){CHECK(!text.text&&!text.length);status=XR_XIR_OK;}
    else CHECK(status!=XR_XIR_OK);
    if(status==XR_XIR_OK) {
        status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){2,10},&text);
        if(status==XR_XIR_OK)CHECK(!strcmp(text.text,"parameter action: fn(ref i64) -> i64 [root: UNRESOLVED]"));
    }
done:
    xlsp_source_hover_free(&text);xlsp_source_snapshot_free(snapshot);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);return status;
#undef HOVER_SOURCE_TYPE
#undef HOVER_DISPLAY_TYPE
}
static void lsp_hover_typed_matrix(const char *root) {
    size_t sites=0;XrCompileResourceStats required={0};
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirStatus status=lsp_hover_typed_once(root,lsp_limits(),probe?NULL:&required);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
        lsp_zero();
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(lsp_hover_typed_once(root,limits,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));lsp_zero();
    }
}
static void lsp_hover_cases(const char *root) {
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    char uri[XR_TEST_PATH_MAX],lib[XR_TEST_PATH_MAX];lsp_uri(uri,sizeof(uri),root,"root.xr");lsp_uri(lib,sizeof(lib),root,"lib.xr");
    XlspSourceDocument docs[2]={{uri,source_root_text,strlen(uri),sizeof(source_root_text)-1,1},
        {lib,source_library_text,strlen(lib),sizeof(source_library_text)-1,7}};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceSnapshot *snapshot=NULL;CHECK(xlsp_source_snapshot_build(&request,docs,2,0,&snapshot,NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);session=NULL;xr_compile_resources_release(context.resources);
    /* Snapshot pins the same resource ledger after caller/Session demise. */
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;XrJsonValue *json=NULL;
        XlspSourceSnapshot *same[2]={snapshot,snapshot};
        XrXirStatus status=xlsp_source_hover_many_json(context.resources,same,2,uri,strlen(uri),(XrLspPosition){1,43},&json);
        if(!probe){CHECK(status==XR_XIR_OK);sites=lsp_json_calls;CHECK(sites);lsp_hover_json_facts(json);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&!json&&lsp_json_injected);
        xjson_free(json);CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrJsonValue *occupied=xjson_new_number(73),*saved=occupied;CHECK(occupied);size_t before=lsp_json_calls;
    CHECK(xlsp_source_hover_many_json(context.resources,&snapshot,1,uri,strlen(uri),(XrLspPosition){1,43},&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==saved&&lsp_json_calls==before);
    xjson_free(occupied);
    XlspSourceHover text={0};CHECK(xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){1,43},&text)==XR_XIR_OK);
    XrJsonValue *json=NULL;CHECK(xlsp_source_hover_many_json(context.resources,&snapshot,1,uri,strlen(uri),(XrLspPosition){1,43},&json)==XR_XIR_OK);
    xlsp_source_snapshot_free(snapshot);snapshot=NULL;
    /* Hover and JSON each own text after the semantic producer is gone. */
    CHECK(!strcmp(text.text,"export fn value() -> i64")&&text.length==sizeof("export fn value() -> i64")-1);lsp_hover_json_facts(json);
    xlsp_source_hover_free(&text);lsp_zero();xjson_free(json);CHECK(!lsp_json_live&&!lsp_json_bytes);

    lsp_hover_typed_matrix(root);
}
