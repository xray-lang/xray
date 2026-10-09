static const char semantic_source[]="/* \xf0\x9f\x98\x80 */ export fn identity(x:i64)->i64 { return x }\n";
static void semantic_facts(const XlspSourceTokens *tokens,uint32_t row) {
    size_t count=0;const XlspSourceToken *t=xlsp_source_tokens_items(tokens,&count);CHECK(t&&count==3);
    const uint32_t columns[]={19,28,49},lengths[]={8,1,1},types[]={12,7,7},mods[]={3,5,4};
    for(size_t i=0;i<count;++i)CHECK(t[i].line==row&&t[i].column==columns[i]&&t[i].length==lengths[i]&&t[i].type==types[i]&&t[i].modifiers==mods[i]);
}
static void semantic_words(XrJsonValue *array,const uint32_t *expected,size_t count) {
    CHECK(array&&array->type==XR_JSON_ARRAY&&(size_t)array->as.array.count==count);
    for(size_t i=0;i<count;++i)CHECK(array->as.array.items[i]->type==XR_JSON_NUMBER&&array->as.array.items[i]->as.number==expected[i]);
}
static XrXirStatus semantic_json_run(XrCompileResources *resources,const XlspSourceTokens *old,
    const XlspSourceTokens *fresh,XrJsonValue **last) {
    XrJsonValue *json=NULL;XrXirStatus status=xlsp_source_tokens_json(resources,old,NULL,7,false,NULL,&json);
    static const uint32_t full[]={0,19,8,12,3,0,9,1,7,5,0,21,1,7,4};
    static const uint32_t moved[]={1,19,8,12,3},selected[]={1,28,1,7,5};
    if(status!=XR_XIR_OK){CHECK(!json);return status;}
    CHECK(!strcmp(xjson_get_string(json,"resultId"),"7"));semantic_words(xjson_get(json,"data"),full,15);xjson_free(json);json=NULL;
    status=xlsp_source_tokens_json(resources,fresh,old,8,true,NULL,&json);if(status!=XR_XIR_OK){CHECK(!json);return status;}
    XrJsonValue *edits=xjson_get(json,"edits");CHECK(edits&&edits->type==XR_JSON_ARRAY&&edits->as.array.count==1);
    XrJsonValue *edit=edits->as.array.items[0];CHECK(xjson_get_int(edit,"start")==0&&xjson_get_int(edit,"deleteCount")==5);
    semantic_words(xjson_get(edit,"data"),moved,5);xjson_free(json);json=NULL;
    status=xlsp_source_tokens_json(resources,fresh,fresh,9,true,NULL,&json);if(status!=XR_XIR_OK){CHECK(!json);return status;}
    edits=xjson_get(json,"edits");CHECK(edits&&edits->type==XR_JSON_ARRAY&&!edits->as.array.count);xjson_free(json);json=NULL;
    XrLspRange range={{1,28},{1,29}};
    status=xlsp_source_tokens_json(resources,fresh,NULL,0,false,&range,&json);if(status!=XR_XIR_OK){CHECK(!json);return status;}
    CHECK(!xjson_get(json,"resultId"));semantic_words(xjson_get(json,"data"),selected,5);xjson_free(json);json=NULL;
    /* Missing/mismatched cache passes NULL previous and returns a real full
     * result under delta protocol; it never edits an unrelated client's base. */
    status=xlsp_source_tokens_json(resources,fresh,NULL,10,true,NULL,&json);if(status!=XR_XIR_OK){CHECK(!json);return status;}
    CHECK(xjson_get(json,"data")&&!xjson_get(json,"edits"));
    XrJsonValue *saved=json;size_t before=lsp_json_calls;
    CHECK(xlsp_source_tokens_json(resources,fresh,old,11,true,NULL,&json)==XR_XIR_BAD_STRUCTURE&&json==saved&&lsp_json_calls==before);
    *last=json;return XR_XIR_OK;
}
static XrXirStatus semantic_once(const char *root,XrCompileResourceLimits limits,XrCompileResourceStats *stats,bool json_fi) {
    XrXirCompileContext context={0};XrCompilerSession *session=NULL;XlspSourceSnapshot *snap[2]={NULL,NULL};XlspSourceTokens *tokens[2]={NULL,NULL};XrJsonValue *json=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    context.limits=xr_xir_compile_default_limits();XrCompilerSessionStatus opened=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char uri[XR_TEST_PATH_MAX],new_text[sizeof(semantic_source)+1];lsp_uri(uri,sizeof(uri),root,"semantic.xr");new_text[0]='\n';memcpy(new_text+1,semantic_source,sizeof(semantic_source));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    for(unsigned i=0;i<2&&status==XR_XIR_OK;++i) {
        XlspSourceDocument doc={uri,i?new_text:semantic_source,strlen(uri),sizeof(semantic_source)-1+i,(int64_t)i+1};
        status=xlsp_source_snapshot_build(&request,&doc,1,0,&snap[i],NULL,NULL);
    }
    xr_compile_session_free(session);session=NULL;memset(new_text,'?',sizeof(new_text));
    for(unsigned i=0;i<2&&status==XR_XIR_OK;++i) {
        status=xlsp_source_semantic_tokens(snap[i],uri,strlen(uri),&tokens[i]);
        if(status==XR_XIR_OK) {
            semantic_facts(tokens[i],i);XlspSourceTokens *occupied=tokens[i];size_t before=source_fixture_compile_attempts;
            CHECK(xlsp_source_semantic_tokens(snap[i],uri,strlen(uri),&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==tokens[i]&&source_fixture_compile_attempts==before);
        } else CHECK(!tokens[i]);
    }
    if(status==XR_XIR_OK) {
        status=xlsp_source_semantic_range(snap[1],uri,strlen(uri),(XrLspRange){{1,28},{1,29}});
        if(status==XR_XIR_OK)CHECK(xlsp_source_semantic_range(snap[1],uri,strlen(uri),(XrLspRange){{1,4},{1,5}})==XR_XIR_BAD_STRUCTURE);
    }
    xlsp_source_snapshot_free(snap[0]);xlsp_source_snapshot_free(snap[1]);
    if(status==XR_XIR_OK) {
        semantic_facts(tokens[0],0);semantic_facts(tokens[1],1);
        if(json_fi) {
            size_t sites=0;
            for(size_t probe=0;probe<=sites;++probe) {
                lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
                XrXirStatus encoded=semantic_json_run(context.resources,tokens[0],tokens[1],&json);
                if(!probe){CHECK(encoded==XR_XIR_OK);sites=lsp_json_calls;CHECK(sites);}
                else CHECK(encoded==XR_XIR_OUT_OF_MEMORY&&!json&&lsp_json_injected);
                xjson_free(json);json=NULL;CHECK(!lsp_json_live&&!lsp_json_bytes);
            }
            lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
        }
        status=semantic_json_run(context.resources,tokens[0],tokens[1],&json);
    }
    xlsp_source_tokens_free(tokens[0]);xlsp_source_tokens_free(tokens[1]);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);lsp_zero();
    if(status==XR_XIR_OK){CHECK(!strcmp(xjson_get_string(json,"resultId"),"10"));CHECK(xjson_get(json,"data")->as.array.count==15);}
    xjson_free(json);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void lsp_semantic_cases(const char *root) {
    size_t sites=0;XrCompileResourceStats needed={0};
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirStatus status=semantic_once(root,lsp_limits(),probe?NULL:&needed,false);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);lsp_zero();
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={needed.allocated_bytes,needed.peak_bytes,needed.work};uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(semantic_once(root,limits,NULL,false)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));lsp_zero();
    }
    CHECK(semantic_once(root,lsp_limits(),NULL,true)==XR_XIR_OK);lsp_zero();
    /* Exact method roles now come from the owned Source syntax sidecar. */
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    char uri[XR_TEST_PATH_MAX];lsp_uri(uri,sizeof(uri),root,"method.xr");
    static const char method[]="struct S { f()->i64 { return 1 } }\n";
    XlspSourceDocument document={uri,method,strlen(uri),sizeof(method)-1,1};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceSnapshot *snapshot=NULL;CHECK(xlsp_source_snapshot_build(&request,&document,1,0,&snapshot,NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);xr_compile_resources_release(context.resources);XlspSourceTokens *tokens=NULL;
    CHECK(xlsp_source_semantic_tokens(snapshot,uri,strlen(uri),&tokens)==XR_XIR_OK&&tokens);
    size_t count=0;const XlspSourceToken *items=xlsp_source_tokens_items(tokens,&count);
    CHECK(count==2&&items[0].line==0&&items[0].column==7&&items[0].length==1&&items[0].type==5&&items[0].modifiers==3);
    CHECK(items[1].line==0&&items[1].column==11&&items[1].length==1&&items[1].type==13&&items[1].modifiers==3);
    xlsp_source_snapshot_free(snapshot);xlsp_source_tokens_free(tokens);lsp_zero();
}
