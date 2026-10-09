/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_open.h"
#include "app/cli/xcli_canonical_source.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
static const char open_root[]="import { value } from \"./lib\"\nexport fn result()->i64 { /* \xf0\x9f\x98\x80 */ return value() }\n";
static const char open_library[]="/* \xf0\x9f\x98\x80 */ export fn value()->i64 { return 41 }\n";
static const char open_bad_syntax[]="/* \xf0\x9f\x98\x80 */ var *** = ;\n";
static const char open_bad_type[]="export fn result()->i64 { return true }\n";
static void open_diagnostic_facts(XrJsonValue *json,unsigned mode) {
    CHECK(json&&json->type==XR_JSON_ARRAY);
    if(!mode){CHECK(json->as.array.count==0);return;}
    CHECK(json->as.array.count>0);
    XrJsonValue *d=json->as.array.items[0],*r=xjson_get_object(d,"range"),*start=xjson_get_object(r,"start"),*end=xjson_get_object(r,"end");
    CHECK(start&&end&&xjson_get_int(d,"severity")==1&&!strcmp(xjson_get_string(d,"source"),"xray"));
    CHECK(xjson_get_string(d,"message")&&xjson_get_string(d,"message")[0]);
    if(mode==1) {
        /* Literal independent oracle: three prefix ASCII units, emoji two,
         * four comment-tail units, 'var ' four => offending star at 13. */
        CHECK(xjson_get_int(start,"line")==0&&xjson_get_int(start,"character")==13);
        CHECK(xjson_get_int(end,"line")==0&&xjson_get_int(end,"character")==14);
    } else CHECK(xjson_get_int(start,"line")==0&&xjson_get_int(end,"line")==0);
}
static XrXirStatus open_once(const char *root_uri,const char *lib_uri,unsigned mode,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XlspSourceOpen *open=NULL;XlspSourceBuffer *buffer=NULL;
    bool caller_lease=true;
    XlspSyntaxSnapshot *syntax=NULL;XlspSourceWorkspace *workspace=NULL;XrJsonValue *diagnostics=NULL,*definition=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    char root[sizeof(open_root)],lib[sizeof(open_library)];
    const char *source=mode==0?open_root:mode==1?open_bad_syntax:open_bad_type;
    CHECK(strlen(source)<sizeof(root));strcpy(root,source);memcpy(lib,open_library,sizeof(lib));
    XlspSourceDocument inputs[2]={{root_uri,root,strlen(root_uri),strlen(root),1},{lib_uri,lib,strlen(lib_uri),sizeof(lib)-1,7}};
    XrXirStatus status=xlsp_source_open_prepare(&context,inputs,2,0,&open);
    if(status==XR_XIR_OK) {
        XlspSourceOpen *occupied=open;size_t attempts=runtime_attempts;
        CHECK(xlsp_source_open_prepare(&context,inputs,2,0,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==open&&attempts==runtime_attempts);
        xr_compile_resources_release(resources);caller_lease=false;memset(&context,0,sizeof(context));
    }
    memset(root,'?',sizeof(root));memset(lib,'?',sizeof(lib));memset(inputs,0,sizeof(inputs));
    if(status!=XR_XIR_OK){CHECK(!open);goto done;}
    CHECK(open&&!strcmp(xlsp_source_open_uri(open),root_uri));
    CHECK(xlsp_source_open_query_status(open)==(mode==2?XR_XIR_BAD_TYPE:mode==1?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
    buffer=xlsp_source_open_take_buffer(open);syntax=xlsp_source_open_take_syntax(open);workspace=xlsp_source_open_take_workspace(open);
    CHECK(buffer&&syntax&&((workspace!=NULL)==(mode==0)));
    CHECK(!xlsp_source_open_take_buffer(open)&&!xlsp_source_open_take_syntax(open)&&!xlsp_source_open_take_workspace(open));
    CHECK(xlsp_source_syntax_status(syntax)==(mode==1?XR_PARSE_RECOVERED:XR_PARSE_OK));
    status=xlsp_source_open_diagnostics_json(open,buffer,syntax,&diagnostics);
    if(status==XR_XIR_OK)open_diagnostic_facts(diagnostics,mode);else CHECK(!diagnostics);
    if(status==XR_XIR_OK&&workspace) {
        status=xlsp_source_workspace_navigation(workspace,root_uri,strlen(root_uri),(XrLspPosition){1,43},0,false,&definition);
        if(status==XR_XIR_OK)CHECK(definition&&!strcmp(xjson_get_string(definition,"uri"),lib_uri));else CHECK(!definition);
    }
done:
    if(stats)*stats=sdk_stats(resources);
    if(caller_lease)xr_compile_resources_release(resources);xlsp_source_open_free(open);xlsp_source_buffer_free(buffer);
    xlsp_source_syntax_free(syntax);xlsp_source_workspace_free(workspace);CHECK(!runtime_live&&!runtime_bytes);
    if(diagnostics)open_diagnostic_facts(diagnostics,mode);
    if(definition)CHECK(!strcmp(xjson_get_string(definition,"uri"),lib_uri));
    xjson_free(diagnostics);xjson_free(definition);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void open_json_cases(const char *root_uri,const char *lib_uri) {
    for(unsigned mode=1;mode<=2;++mode) {
        XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();XrCompileResources *resources=sdk_ledger(&limits);
        XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
        const char *text=mode==1?open_bad_syntax:open_bad_type;
        XlspSourceDocument inputs[2]={{root_uri,text,strlen(root_uri),strlen(text),1},{lib_uri,open_library,strlen(lib_uri),sizeof(open_library)-1,7}};
        XlspSourceOpen *open=NULL;CHECK(xlsp_source_open_prepare(&context,inputs,2,0,&open)==XR_XIR_OK);
        XlspSourceBuffer *buffer=xlsp_source_open_take_buffer(open);XlspSyntaxSnapshot *syntax=xlsp_source_open_take_syntax(open);
        CHECK(!xlsp_source_open_take_workspace(open));xr_compile_resources_release(resources);
        size_t sites=0;
        for(size_t probe=0;probe<=sites;++probe) {
            lsp_json_calls=0;lsp_json_fail=probe?probe-1:SIZE_MAX;lsp_json_injected=false;
            XrJsonValue *out=NULL;XrXirStatus status=xlsp_source_open_diagnostics_json(open,buffer,syntax,&out);
            if(!probe){CHECK(status==XR_XIR_OK);open_diagnostic_facts(out,mode);sites=lsp_json_calls;CHECK(sites);}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&lsp_json_injected&&!out);
            xjson_free(out);CHECK(!lsp_json_live&&!lsp_json_bytes);
        }
        lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
        XrJsonValue *occupied=xjson_new_number(73),*saved=occupied;CHECK(occupied);size_t before=lsp_json_calls;
        CHECK(xlsp_source_open_diagnostics_json(open,buffer,syntax,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==saved&&before==lsp_json_calls);
        xjson_free(occupied);XrJsonValue *out=NULL;
        CHECK(xlsp_source_open_diagnostics_json(open,buffer,syntax,&out)==XR_XIR_OK);
        xlsp_source_open_free(open);xlsp_source_buffer_free(buffer);xlsp_source_syntax_free(syntax);CHECK(!runtime_live&&!runtime_bytes);
        open_diagnostic_facts(out,mode);xjson_free(out);CHECK(!lsp_json_live&&!lsp_json_bytes);
        printf("LSP actual diagnostic JSON mode=%u sites=%zu, occupied/producer death, physical=0/0\n",mode,sites);
    }
}
static int open_run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==3);XrCompileResourceLimits limits=xr_cli_compile_default_resource_limits();
    for(unsigned mode=0;mode<3;++mode) {
        size_t sites=0;XrCompileResourceStats stats={0};
        for(size_t probe=0;probe<=sites;++probe) {
            runtime_attempts=0;runtime_fail_at=probe?probe-1:SIZE_MAX;
            XrXirStatus status=open_once(argv[1],argv[2],mode,limits,probe?NULL:&stats);
            if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts;CHECK(sites);}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>=probe);
            CHECK(!runtime_live&&!runtime_bytes);
        }
        runtime_fail_at=SIZE_MAX;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
            uint64_t *n=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
            CHECK(*n&&*n<INT64_MAX);*n=(uint64_t)((int64_t)*n+delta);
            CHECK(open_once(argv[1],argv[2],mode,exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
        printf("LSP open mode=%u actual compiler sites=%zu, three axes, producer death, physical=0/0\n",mode,sites);
    }
    open_json_cases(argv[1],argv[2]);return 0;
}
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=open_run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
