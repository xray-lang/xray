/* Fixed parser-source oracle; all columns below are literal UTF-16 units. */
static const char outline_text[]=
    "/* \xf0\x9f\x98\x80 */ fn before()->i64 {\n return 1\n}\n"
    "struct Box {\n const value:i64\n answer()->i64 { return this.value }\n}\n"
    "enum Tone {\n Red,\n Blue\n}\n";
static void outline_facts(const XlspSourceOutline *outline) {
    size_t count=0;const XlspSourceSymbol *s=xlsp_source_outline_symbols(outline,&count);CHECK(s&&count==7);
    const char *names[]={"before","Box","value","answer","Tone","Red","Blue"};
    const uint32_t kinds[]={12,5,8,6,10,22,22},parents[]={UINT32_MAX,UINT32_MAX,1,1,UINT32_MAX,4,4};
    const uint32_t rows[]={0,3,4,5,7,8,9},cols[]={12,7,7,1,5,1,1},ends[]={18,10,12,7,9,4,5};
    const uint32_t fullrows[]={2,6,4,5,10,8,9},fullcols[]={1,1,12,36,1,4,5};
    for(size_t i=0;i<count;++i) {
        CHECK(!strcmp(s[i].name,names[i])&&s[i].kind==kinds[i]&&s[i].parent==parents[i]);
        CHECK(s[i].range.start.line==rows[i]&&s[i].range.start.character==cols[i]);
        CHECK(s[i].selection.start.line==rows[i]&&s[i].selection.start.character==cols[i]);
        CHECK(s[i].selection.end.line==rows[i]&&s[i].selection.end.character==ends[i]);
        CHECK(s[i].range.end.line==fullrows[i]&&s[i].range.end.character==fullcols[i]);
    }
}
static void outline_json_facts(XrJsonValue *json) {
    CHECK(json&&json->type==XR_JSON_ARRAY&&json->as.array.count==3);
    CHECK(!strcmp(xjson_get_string(json->as.array.items[0],"name"),"before"));
    XrJsonValue *range=xjson_get_object(json->as.array.items[0],"selectionRange");
    CHECK(xjson_get_int(xjson_get_object(range,"start"),"character")==12);
    XrJsonValue *box=json->as.array.items[1],*tone=json->as.array.items[2];
    CHECK(!strcmp(xjson_get_string(box,"name"),"Box")&&!strcmp(xjson_get_string(tone,"name"),"Tone"));
    XrJsonValue *fields=xjson_get(box,"children"),*variants=xjson_get(tone,"children");
    CHECK(fields&&fields->type==XR_JSON_ARRAY&&fields->as.array.count==2);
    CHECK(variants&&variants->type==XR_JSON_ARRAY&&variants->as.array.count==2);
    CHECK(!strcmp(xjson_get_string(fields->as.array.items[0],"name"),"value"));
    CHECK(!strcmp(xjson_get_string(fields->as.array.items[1],"name"),"answer"));
    CHECK(!strcmp(xjson_get_string(variants->as.array.items[0],"name"),"Red"));
    CHECK(!strcmp(xjson_get_string(variants->as.array.items[1],"name"),"Blue"));
}
static XrXirStatus outline_once(const char *root,XrCompileResourceLimits limits,XrCompileResourceStats *stats,bool json_fi) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *syntax=NULL;XlspSourceOutline *outline=NULL;
    XrJsonValue *json=NULL;XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    XrXirStatus status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char uri[XR_TEST_PATH_MAX],text[sizeof(outline_text)];lsp_uri(uri,sizeof(uri),root,"outline.xr");memcpy(text,outline_text,sizeof(text));
    XlspSourceDocument input={uri,text,strlen(uri),sizeof(text)-1,19};
    if(status==XR_XIR_OK) {
        XrParseStatus parsed=xlsp_source_syntax_build(session,&input,&syntax);
        status=parsed==XR_PARSE_OK?XR_XIR_OK:parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_session_free(session);session=NULL;memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));
    if(status==XR_XIR_OK)status=xlsp_source_syntax_outline(syntax,&outline);
    if(status==XR_XIR_OK) {
        outline_facts(outline);XlspSourceOutline *occupied=outline;size_t before=source_fixture_compile_attempts;
        CHECK(xlsp_source_syntax_outline(syntax,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==outline&&before==source_fixture_compile_attempts);
    } else CHECK(!outline);
    xlsp_source_syntax_free(syntax);syntax=NULL;
    if(status==XR_XIR_OK) {
        outline_facts(outline); /* Full syntax/URI/text/Session producer is dead. */
        if(json_fi) {
            size_t sites=0;
            for(size_t probe=0;probe<=sites;++probe) {
                lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
                XrXirStatus encoded=xlsp_source_outline_json(outline,&json);
                if(!probe){CHECK(encoded==XR_XIR_OK);outline_json_facts(json);sites=lsp_json_calls;CHECK(sites);}
                else CHECK(encoded==XR_XIR_OUT_OF_MEMORY&&!json&&lsp_json_injected);
                xjson_free(json);json=NULL;CHECK(!lsp_json_live&&!lsp_json_bytes);
            }
            lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
        }
        status=xlsp_source_outline_json(outline,&json);
        if(status==XR_XIR_OK) {
            outline_json_facts(json);XrJsonValue *saved=json;size_t before=lsp_json_calls;
            CHECK(xlsp_source_outline_json(outline,&json)==XR_XIR_BAD_STRUCTURE&&json==saved&&before==lsp_json_calls);
        } else CHECK(!json);
    }
    xlsp_source_outline_free(outline);
    if(stats)CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(resources);lsp_zero();
    if(status==XR_XIR_OK)outline_json_facts(json);xjson_free(json);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void lsp_outline_cases(const char *root) {
    size_t sites=0;XrCompileResourceStats needed={0};
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirStatus status=outline_once(root,lsp_limits(),probe?NULL:&needed,false);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
        lsp_zero();
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={needed.allocated_bytes,needed.peak_bytes,needed.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(outline_once(root,limits,NULL,false)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));lsp_zero();
    }
    CHECK(outline_once(root,lsp_limits(),NULL,true)==XR_XIR_OK);lsp_zero();
}
