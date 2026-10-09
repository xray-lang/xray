/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_syntax.h"
/* Literal-only oracle, fixed before execution: functions [0,2]/[3,8],
 * nested if [4,6]. In recovery mode the one extra invalid line shifts the
 * second function and its if by one. No runtime span refresh. */
static const char syntax_head[]="fn before()->i64 {\n return 1\n}\n";
static const char syntax_tail[]="fn after()->i64 {\n if (true) {\n return 2\n }\n return 3\n}\n";
static void syntax_folding_facts(XrJsonValue *json,bool recovered) {
    CHECK(json&&json->type==XR_JSON_ARRAY&&json->as.array.count==3);
    const int start[3]={0,3+(int)recovered,4+(int)recovered};
    const int end[3]={2,8+(int)recovered,6+(int)recovered};
    for(int i=0;i<3;++i) {
        XrJsonValue *range=json->as.array.items[i];CHECK(range&&range->type==XR_JSON_OBJECT&&range->as.object.count==3);
        CHECK(xjson_get_int(range,"startLine")==start[i]&&xjson_get_int(range,"endLine")==end[i]);
        CHECK(!strcmp(xjson_get_string(range,"kind"),"region"));
    }
}
static XrXirStatus syntax_once(const char *root,bool recovered,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *syntax=NULL;
    XrJsonValue *json=NULL;XrXirStatus status=XR_XIR_OK;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    char uri[XR_TEST_PATH_MAX],text[512];lsp_uri(uri,sizeof(uri),root,"unsaved.xr");
    int n=snprintf(text,sizeof(text),"%s%s%s",syntax_head,recovered?"var *** = ;\n":"",syntax_tail);
    CHECK(n>0&&(size_t)n<sizeof(text));
    XlspSourceDocument input={uri,text,strlen(uri),(size_t)n,7};
    XrParseStatus parsed=xlsp_source_syntax_build(session,&input,&syntax);
    if(parsed==XR_PARSE_OK||parsed==XR_PARSE_RECOVERED) {
        CHECK(parsed==(recovered?XR_PARSE_RECOVERED:XR_PARSE_OK));
        CHECK(xlsp_source_syntax_status(syntax)==parsed&&xlsp_source_syntax_ast(syntax));
        const XlspSyntaxDiagnostic *diagnostic=xlsp_source_syntax_diagnostics(syntax);
        if(recovered)CHECK(diagnostic&&diagnostic->message&&diagnostic->message[0]&&diagnostic->line==4);
        else CHECK(!diagnostic);
    } else {CHECK(!syntax);status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;}
    memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));memset(&input,0,sizeof(input));
    xr_compile_session_free(session);session=NULL;
    if(status==XR_XIR_OK)status=xlsp_source_syntax_folding_json(syntax,&json);
    if(status==XR_XIR_OK)syntax_folding_facts(json,recovered);else CHECK(!json);
    if(stats)CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
done:
    xlsp_source_syntax_free(syntax);xr_compile_session_free(session);xr_compile_resources_release(resources);
    lsp_zero(); /* JSON remains alive and independent of all syntax owners. */
    if(status==XR_XIR_OK)syntax_folding_facts(json,recovered);
    xjson_free(json);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void syntax_json_cases(const char *root) {
    XrCompileResourceLimits limits=lsp_limits();XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    char uri[XR_TEST_PATH_MAX],text[512];lsp_uri(uri,sizeof(uri),root,"unsaved.xr");
    int n=snprintf(text,sizeof(text),"%s%s",syntax_head,syntax_tail);CHECK(n>0&&(size_t)n<sizeof(text));
    XlspSourceDocument input={uri,text,strlen(uri),(size_t)n,9};XlspSyntaxSnapshot *syntax=NULL;
    CHECK(xlsp_source_syntax_build(session,&input,&syntax)==XR_PARSE_OK);
    XlspSyntaxSnapshot *occupied=syntax;size_t before=source_fixture_compile_attempts;
    CHECK(xlsp_source_syntax_build(session,&input,&occupied)==XR_PARSE_BAD_ARGUMENT&&occupied==syntax&&before==source_fixture_compile_attempts);
    input.length=SIZE_MAX;XlspSyntaxSnapshot *bad=NULL;
    CHECK(xlsp_source_syntax_build(session,&input,&bad)==XR_PARSE_BAD_ARGUMENT&&!bad);input.length=(size_t)n;
    text[3]=0;CHECK(xlsp_source_syntax_build(session,&input,&bad)==XR_PARSE_BAD_ARGUMENT&&!bad);
    xr_compile_session_free(session);xr_compile_resources_release(resources);
    memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
        XrJsonValue *json=NULL;XrXirStatus status=xlsp_source_syntax_folding_json(syntax,&json);
        if(!probe){CHECK(status==XR_XIR_OK);syntax_folding_facts(json,false);sites=lsp_json_calls;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected&&!json);
        xjson_free(json);CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrJsonValue *json=xjson_new_number(73),*saved=json;CHECK(json);before=lsp_json_calls;
    CHECK(xlsp_source_syntax_folding_json(syntax,&json)==XR_XIR_BAD_STRUCTURE&&json==saved&&before==lsp_json_calls);
    xjson_free(json);xlsp_source_syntax_free(syntax);lsp_zero();CHECK(!lsp_json_live&&!lsp_json_bytes);
    printf("LSP syntax JSON fresh sites=%zu; physical=0/0\n",sites);
}
static void lsp_syntax_cases(const char *root) {
    for(unsigned mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats required={0};
        for(size_t probe=0;probe<=sites;++probe) {
            source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
            XrXirStatus status=syntax_once(root,mode!=0,lsp_limits(),probe?NULL:&required);
            if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
            lsp_zero();
        }
        source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
            CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            CHECK(syntax_once(root,mode!=0,limits,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));lsp_zero();
        }
        printf("LSP syntax mode=%u actual sites=%zu; three axes; physical=0/0\n",mode,sites);
    }
    syntax_json_cases(root);
}
