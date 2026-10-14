/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef LSP_OPEN_DIAGNOSTIC_ROUTING_H
#define LSP_OPEN_DIAGNOSTIC_ROUTING_H
static const char routing_bad_type[]=
    "/* \xf0\x9f\x98\x80 */\r\nexport fn value()->i64 { /* \xf0\x9f\x98\x80 */ return true }\r\n";
static void routing_facts(XrJsonValue *params,const char *root_uri,const char *lib_uri,unsigned mode) {
    CHECK(params&&params->type==XR_JSON_OBJECT);
    CHECK(!strcmp(xjson_get_string(params,"uri"),mode==2?root_uri:lib_uri));
    CHECK(xjson_get_int(params,"version")== (mode==2?1:7));
    XrJsonValue *array=xjson_get_array(params,"diagnostics");
    if(mode==2){open_diagnostic_facts(array,1);return;}
    CHECK(array&&array->type==XR_JSON_ARRAY&&array->as.array.count==1);
    XrJsonValue *diagnostic=array->as.array.items[0],*range=xjson_get_object(diagnostic,"range");
    XrJsonValue *start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
    CHECK(xjson_get_int(start,"line")==1&&xjson_get_int(end,"line")==1);
    /* The invalid boolean follows 28 ASCII units, a two-unit emoji, four
     * comment-tail units and seven units of "return ". CRLF is one line. */
    CHECK(xjson_get_int(start,"character")==41&&xjson_get_int(end,"character")==41);
    CHECK(!strcmp(xjson_get_string(diagnostic,"message"),"expression cannot satisfy its declared type"));
    CHECK(xjson_get_int(diagnostic,"severity")==1&&!strcmp(xjson_get_string(diagnostic,"source"),"xray"));
}
static XrXirStatus routing_once(const char *root_uri,const char *lib_uri,unsigned mode,
    XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XlspSourceOpen *open=NULL;
    bool caller_lease=true;
    XlspSourceBuffer *buffer=NULL;XlspSyntaxSnapshot *syntax=NULL;XrJsonValue *params=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    char root[sizeof(open_root)],library[sizeof(routing_bad_type)];
    const char *root_text=mode==2?open_bad_syntax:open_root;
    const char *library_text=mode==3?open_bad_syntax:routing_bad_type;
    strcpy(root,root_text);strcpy(library,library_text);
    size_t root_uri_length=strlen(root_uri),lib_uri_length=strlen(lib_uri);
    char *root_uri_copy=xr_strdup(root_uri),*lib_uri_copy=xr_strdup(lib_uri);CHECK(root_uri_copy&&lib_uri_copy);
    XlspSourceDocument inputs[2]={{root_uri_copy,root,root_uri_length,strlen(root),1},
        {lib_uri_copy,library,lib_uri_length,strlen(library),7}};
    size_t entry=0;
    if(mode==1){XlspSourceDocument saved=inputs[0];inputs[0]=inputs[1];inputs[1]=saved;entry=1;}
    XrXirStatus status=xlsp_source_open_prepare(&context,inputs,2,entry,&open);
    memset(root_uri_copy,'?',root_uri_length);memset(lib_uri_copy,'?',lib_uri_length);
    xr_free(root_uri_copy);xr_free(lib_uri_copy);
    memset(root,'?',sizeof(root));memset(library,'?',sizeof(library));memset(inputs,0,sizeof(inputs));
    if(status!=XR_XIR_OK){CHECK(!open);goto done;}
    xr_compile_resources_release(resources);caller_lease=false;memset(&context,0,sizeof(context));
    buffer=xlsp_source_open_take_buffer(open);syntax=xlsp_source_open_take_syntax(open);
    CHECK(buffer&&syntax&&!xlsp_source_open_take_workspace(open));
    status=xlsp_source_open_diagnostics_params_json(open,buffer,syntax,&params);
    if(status==XR_XIR_OK){CHECK(mode!=3);routing_facts(params,root_uri,lib_uri,mode);}
    else CHECK(!params);
done:
    if(stats)*stats=sdk_stats(resources);
    if(caller_lease)xr_compile_resources_release(resources);xlsp_source_open_free(open);
    xlsp_source_buffer_free(buffer);xlsp_source_syntax_free(syntax);CHECK(!runtime_live&&!runtime_bytes);
    if(params)routing_facts(params,root_uri,lib_uri,mode);
    xjson_free(params);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void routing_json_cases(const char *root_uri,const char *lib_uri) {
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    XrCompileResources *resources=sdk_ledger(&limits);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XlspSourceDocument inputs[2]={{root_uri,open_root,strlen(root_uri),sizeof(open_root)-1,1},
        {lib_uri,routing_bad_type,strlen(lib_uri),sizeof(routing_bad_type)-1,7}};
    XlspSourceOpen *open=NULL;CHECK(xlsp_source_open_prepare(&context,inputs,2,0,&open)==XR_XIR_OK);
    XlspSourceBuffer *buffer=xlsp_source_open_take_buffer(open);XlspSyntaxSnapshot *syntax=xlsp_source_open_take_syntax(open);
    CHECK(!xlsp_source_open_take_workspace(open));xr_compile_resources_release(resources);
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
        XrJsonValue *params=NULL;XrXirStatus status=xlsp_source_open_diagnostics_params_json(open,buffer,syntax,&params);
        if(!probe){CHECK(status==XR_XIR_OK);routing_facts(params,root_uri,lib_uri,0);sites=lsp_json_calls;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected&&!params);
        xjson_free(params);CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrJsonValue *occupied=xjson_new_number(73),*saved=occupied;CHECK(occupied);size_t before=lsp_json_calls;
    CHECK(xlsp_source_open_diagnostics_params_json(open,buffer,syntax,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==saved&&before==lsp_json_calls);
    xjson_free(occupied);XrJsonValue *params=NULL;
    CHECK(xlsp_source_open_diagnostics_params_json(open,buffer,syntax,&params)==XR_XIR_OK);
    xlsp_source_open_free(open);xlsp_source_buffer_free(buffer);xlsp_source_syntax_free(syntax);CHECK(!runtime_live&&!runtime_bytes);
    routing_facts(params,root_uri,lib_uri,0);xjson_free(params);CHECK(!lsp_json_live&&!lsp_json_bytes);
    printf("LSP routed diagnostic actual JSON sites=%zu, occupied/producer death, physical=0/0\n",sites);
}
static void routing_run(const char *root_uri,const char *lib_uri,bool faults) {
    XrCompileResourceLimits limits=xr_source_compile_default_resource_limits();
    for(unsigned mode=0;mode<4;++mode) {
        XrXirStatus expected=mode==3?XR_XIR_UNRESOLVED:XR_XIR_OK;
        size_t sites=0;XrCompileResourceStats stats={0};
        for(size_t probe=0;probe<=sites;++probe) {
            runtime_attempts=0;runtime_fail_at=probe?probe-1:SIZE_MAX;
            XrXirStatus status=routing_once(root_uri,lib_uri,mode,limits,probe?NULL:&stats);
            if(!probe){CHECK(status==expected);if(faults)sites=runtime_attempts;}
            else {
                if(status!=XR_XIR_OUT_OF_MEMORY||runtime_attempts<probe)
                    fprintf(stderr,"LSP routed fault mode=%u probe=%zu/%zu status=%u attempts=%zu\n",mode,probe,sites,(unsigned)status,runtime_attempts);
                CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>=probe);
            }
        }
        runtime_fail_at=SIZE_MAX;
        if(faults)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
            uint64_t *n=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
            CHECK(*n&&*n<INT64_MAX);*n=(uint64_t)((int64_t)*n+delta);
            CHECK(routing_once(root_uri,lib_uri,mode,exact,NULL)==(delta<0?XR_XIR_BUDGET:expected));
        }
        printf("LSP routed diagnostic mode=%u compiler sites=%zu, owned params, physical=0/0\n",mode,sites);
    }
    routing_json_cases(root_uri,lib_uri);
}
#endif // LSP_OPEN_DIAGNOSTIC_ROUTING_H
