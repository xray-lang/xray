/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static const char closed_hover_root[]="import { Shade } from \"./closed_palette\"\nvar selected = Shade.Light\n";
static const char closed_hover_other[]="import { Shade } from \"./other_palette\"\nvar selected = Shade.Light\n";
static void closed_hover_file(const char *path,const char *text) {
    FILE *file=fopen(path,"wb");CHECK(file);size_t length=strlen(text);
    CHECK(fwrite(text,1,length,file)==length&&fclose(file)==0);
}
static XrXirStatus closed_hover_snapshot(const char *root,const char *uri,const char *text,
    XrXirCompileContext *context,XlspSourceSnapshot **output) {
    XrCompilerSession *session=NULL;XrCompilerSessionStatus made=xr_compile_session_new(context->resources,&session);
    if(made!=XR_COMPILER_SESSION_OK)return made==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceDocument document={uri,text,strlen(uri),strlen(text),7};
    XrXirStatus status=xlsp_source_snapshot_build(&request,&document,1,0,output,NULL,NULL);
    xr_compile_session_free(session);return status;
}
static XrXirStatus closed_hover_query(XrCompileResources *resources,XlspSourceSnapshot **snapshots,const char *uri) {
    XrJsonValue *json=NULL;
    XrXirStatus status=xlsp_source_hover_many_json(resources,snapshots,2,uri,strlen(uri),(XrLspPosition){1,16},&json);
    if(status==XR_XIR_OK) {
        CHECK(json&&json->type==XR_JSON_OBJECT&&json->as.object.count==1&&!xjson_get(json,"range"));
        CHECK(!strcmp(xjson_get_string(xjson_get_object(json,"contents"),"value"),"export enum Shade"));
    } else CHECK(!json);
    xjson_free(json);return status;
}
static void closed_hover_query_fi(XrCompileResources *resources,XlspSourceSnapshot **snapshots,const char *uri) {
    size_t live=source_fixture_compile_live,bytes=source_fixture_compile_bytes,sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirStatus status=closed_hover_query(resources,snapshots,uri);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else {
            if(status!=XR_XIR_OUT_OF_MEMORY||!source_fixture_compile_injected)fprintf(stderr,"Closed hover FI probe%zu status%u injected%u attempts%zu\n",probe,(unsigned)status,source_fixture_compile_injected?1u:0u,source_fixture_compile_attempts);
            CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
        }
        CHECK(source_fixture_compile_live==live&&source_fixture_compile_bytes==bytes&&!lsp_json_live&&!lsp_json_bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;size_t source_sites=sites;sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
        XrXirStatus status=closed_hover_query(resources,snapshots,uri);
        if(!probe){CHECK(status==XR_XIR_OK);sites=lsp_json_calls;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected);
        CHECK(source_fixture_compile_live==live&&source_fixture_compile_bytes==bytes&&!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    printf("Closed hover query renderer Source FI=%zu JSON FI=%zu; construction FI remains in original full gates\n",source_sites,sites);
}
static XrXirStatus closed_hover_once(const char *root,XrCompileResourceLimits limits,unsigned mode,XrCompileResourceStats *stats) {
    XrXirCompileContext context={0},foreign={0};XlspSourceSnapshot *snapshots[2]={0};XrJsonValue *survivor=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&context.resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    context.limits=xr_xir_compile_default_limits();foreign.limits=context.limits;
    char uri[XR_TEST_PATH_MAX],library[XR_TEST_PATH_MAX],other[XR_TEST_PATH_MAX],text[sizeof(closed_hover_root)];
    lsp_uri(uri,sizeof(uri),root,"closed_root.xr");memcpy(text,closed_hover_root,sizeof(text));
    CHECK(snprintf(library,sizeof(library),"%s/closed_palette.xr",root)>0&&snprintf(other,sizeof(other),"%s/other_palette.xr",root)>0);
    static const char palette[]="/* \xf0\x9f\x98\x80 A */ export enum Shade { Light, Dark }\n";
    closed_hover_file(library,palette);closed_hover_file(other,palette);
    XrXirStatus status=closed_hover_snapshot(root,uri,text,&context,&snapshots[0]);
    if(status==XR_XIR_OK&&mode==2)closed_hover_file(library,"/* \xf0\x9f\x98\x80 B */ export enum Shade { Light, Dark }\n");
    if(status==XR_XIR_OK&&mode==3) {
        made=xr_compile_resources_new(&limits,&foreign.resources);
        if(made!=XR_COMPILE_RESOURCE_OK)status=made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    }
    if(status==XR_XIR_OK)status=closed_hover_snapshot(root,uri,mode==1?closed_hover_other:text,mode==3?&foreign:&context,&snapshots[1]);
    memset(text,'?',sizeof(text));
    /* Caller input, Sessions and actual files die before the query. */
    CHECK(xr_test_unlink(library)==0&&xr_test_unlink(other)==0);
    if(status==XR_XIR_OK&&mode==3) {
        bool equal=true;XrCompileResourceStats before[2],after[2];
        CHECK(xr_compile_resources_stats(context.resources,&before[0])==XR_COMPILE_RESOURCE_OK&&xr_compile_resources_stats(foreign.resources,&before[1])==XR_COMPILE_RESOURCE_OK);
        CHECK(xlsp_source_same_symbol(snapshots[0],snapshots[1],uri,strlen(uri),(XrLspPosition){1,16},&equal)==XR_XIR_BAD_STRUCTURE&&equal);
        CHECK(xr_compile_resources_stats(context.resources,&after[0])==XR_COMPILE_RESOURCE_OK&&xr_compile_resources_stats(foreign.resources,&after[1])==XR_COMPILE_RESOURCE_OK);
        CHECK(before[0].work==after[0].work&&before[1].work==after[1].work);
        CHECK(closed_hover_query(context.resources,snapshots,uri)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_compile_resources_stats(context.resources,&after[0])==XR_COMPILE_RESOURCE_OK&&xr_compile_resources_stats(foreign.resources,&after[1])==XR_COMPILE_RESOURCE_OK);
        CHECK(before[0].work+2==after[0].work&&before[1].work==after[1].work);
    } else if(status==XR_XIR_OK&&mode==4)closed_hover_query_fi(context.resources,snapshots,uri);
    else if(status==XR_XIR_OK&&mode==5)status=xlsp_source_hover_many_json(context.resources,snapshots,2,uri,strlen(uri),(XrLspPosition){1,16},&survivor);
    else if(status==XR_XIR_OK)status=closed_hover_query(context.resources,snapshots,uri);
    xlsp_source_snapshot_free(snapshots[0]);xlsp_source_snapshot_free(snapshots[1]);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(foreign.resources);xr_compile_resources_release(context.resources);lsp_zero();
    if(survivor)CHECK(!strcmp(xjson_get_string(xjson_get_object(survivor,"contents"),"value"),"export enum Shade"));
    xjson_free(survivor);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void closed_hover_versions(const char *root) {
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XlspSourceSnapshot *snapshots[2]={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    char uri[XR_TEST_PATH_MAX],library[XR_TEST_PATH_MAX];lsp_uri(uri,sizeof(uri),root,"closed_root.xr");lsp_uri(library,sizeof(library),root,"closed_palette.xr");
    static const char palette[]="export enum Shade { Light, Dark }\n";
    XlspSourceDocument documents[2]={{uri,closed_hover_root,strlen(uri),sizeof(closed_hover_root)-1,7},{library,palette,strlen(library),sizeof(palette)-1,1}};
    for(unsigned i=0;i<2;++i) {
        XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
        XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};
        XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};documents[1].version=(int64_t)i+1;
        CHECK(xlsp_source_snapshot_build(&request,documents,2,0,&snapshots[i],NULL,NULL)==XR_XIR_OK);xr_compile_session_free(session);
    }
    bool equal=true;CHECK(xlsp_source_same_symbol(snapshots[0],snapshots[1],uri,strlen(uri),(XrLspPosition){1,16},&equal)==XR_XIR_OK&&!equal);
    CHECK(closed_hover_query(context.resources,snapshots,uri)==XR_XIR_BAD_STRUCTURE);
    xlsp_source_snapshot_free(snapshots[0]);xlsp_source_snapshot_free(snapshots[1]);xr_compile_resources_release(context.resources);lsp_zero();
}
static void lsp_hover_closed_cases(const char *root) {
    XrCompileResourceStats required={0};CHECK(closed_hover_once(root,lsp_limits(),0,&required)==XR_XIR_OK);
    /* Identical display text cannot merge distinct governed modules or content. */
    CHECK(closed_hover_once(root,lsp_limits(),1,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(closed_hover_once(root,lsp_limits(),2,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(closed_hover_once(root,lsp_limits(),3,NULL)==XR_XIR_OK);
    CHECK(closed_hover_once(root,lsp_limits(),4,NULL)==XR_XIR_OK);
    CHECK(closed_hover_once(root,lsp_limits(),5,NULL)==XR_XIR_OK);
    closed_hover_versions(root);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(closed_hover_once(root,limits,0,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    puts("Closed dependency hover: owned canonical identity, two-root dedup, content/module conflicts, foreign ledger and exact resource axes physical0/0");
}
