/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_diagnostics.c - Actual parser failures after source and graph death
 */
#include "app/lsp/xlsp_source_open.h"
#include "xir/xxir_source_corpus.h"
#include "module/xmodule_position.h"
#include "toolchain/xcompiler_session.h"
#ifdef XR_OS_WINDOWS
#include "base/xwindows_utf8.h"
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
_Static_assert(sizeof(XrXirSourceDiagnostic)==208,"Source diagnostic layout must remain unchanged");
_Static_assert(offsetof(XrXirSourceDiagnostic,message)==16,"Source diagnostic offsets must remain unchanged");
typedef struct DiagnosticCase {
    const char *root_uri,*lib_uri,*root_path,*lib_path,*directory;
    unsigned mode;size_t source_fault,json_fault;
    XrCompileResourceLimits limits;
    bool budget,report,change_disk;
} DiagnosticCase;
typedef struct DiagnosticMetrics {size_t source,json;bool source_hit,json_hit;XrCompileResourceStats stats;} DiagnosticMetrics;
static const char root_source[]="import { value } from \"./lib\"\nexport fn result()->i64{return value()}\n";
static const char bad_source[]="/* \xf0\x9f\x98\x80 */ var *** = ;\n";
static const char crlf_source[]="/* \xf0\x9f\x98\x80 */\r\n/* \xf0\x9f\x98\x80 */ var *** = ;\r\n";
static const char cr_source[]="/* \xf0\x9f\x98\x80 */\r/* \xf0\x9f\x98\x80 */ var *** = ;\n";
static const char *diagnostic_source(unsigned mode){return mode==2?crlf_source:mode==3?cr_source:bad_source;}
#include "position_cases.h"
#include "lifetime_cases.h"
static void diagnostic_disk(const char *path,const char *text) {
#ifdef XR_OS_WINDOWS
    wchar_t wide[4096];CHECK(xr_win_utf8_to_utf16(path,strlen(path),wide,sizeof(wide)/sizeof(*wide)));
    FILE *file=_wfopen(wide,L"wb");
#else
    FILE *file=fopen(path,"wb");
#endif
    CHECK(file);CHECK(fputs(text,file)>=0);CHECK(!fclose(file));
}
static void diagnostic_json(XrJsonValue *params,const DiagnosticCase *input) {
    CHECK(params&&params->type==XR_JSON_OBJECT);
    CHECK(!strcmp(xjson_get_string(params,"uri"),input->mode==5?input->root_uri:input->lib_uri));
    XrJsonValue *version=xjson_get(params,"version");CHECK(version);
    if(input->mode==0||input->mode==4)CHECK(xjson_get_int(params,"version")==9);
    else if(input->mode==5)CHECK(xjson_get_int(params,"version")==7);
    else CHECK(version->type==XR_JSON_NULL);
    XrJsonValue *array=xjson_get_array(params,"diagnostics");CHECK(array&&array->as.array.count>=1);
    if(input->mode!=5)CHECK(array->as.array.count==1);
    XrJsonValue *diag=xjson_array_get(array,0),*range=xjson_get_object(diag,"range");
    XrJsonValue *start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");CHECK(start&&end);
    CHECK(xjson_get_int(start,"line")==((input->mode==2||input->mode==3)?1:0));
    CHECK(xjson_get_int(end,"line")==xjson_get_int(start,"line"));
    CHECK(xjson_get_int(start,"character")==13&&xjson_get_int(end,"character")== (input->mode==5?14:13));
    CHECK(!strcmp(xjson_get_string(diag,"message"),"expected variable name"));
    CHECK(xjson_get_int(diag,"severity")==1&&!strcmp(xjson_get_string(diag,"source"),"xray"));
}
static XrXirStatus diagnostic_open(const DiagnosticCase *input,XrCompileResources *resources,XrJsonValue **params) {
    char root_uri[4096],lib_uri[4096],root[256],library[256];
    CHECK(strlen(input->root_uri)<sizeof(root_uri)&&strlen(input->lib_uri)<sizeof(lib_uri));
    strcpy(root_uri,input->root_uri);strcpy(lib_uri,input->lib_uri);
    strcpy(root,input->mode==5?bad_source:root_source);strcpy(library,diagnostic_source(input->mode));
    XlspSourceDocument documents[2]={{root_uri,root,strlen(root_uri),strlen(root),7},
        {lib_uri,library,strlen(lib_uri),strlen(library),9}};
    size_t count=(input->mode==0||input->mode==4)?2u:1u;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XlspSourceOpen *open=NULL;
    XrXirStatus status=xlsp_source_open_prepare(&context,documents,count,0,&open);
    memset(root_uri,'?',sizeof(root_uri));memset(lib_uri,'?',sizeof(lib_uri));
    memset(root,'?',sizeof(root));memset(library,'?',sizeof(library));memset(documents,0,sizeof(documents));
    if(status!=XR_XIR_OK){CHECK(!open);return status;}
    CHECK(xlsp_source_open_query_status(open)==XR_XIR_BAD_STRUCTURE);
    XlspSourceBuffer *buffer=xlsp_source_open_take_buffer(open);XlspSyntaxSnapshot *syntax=xlsp_source_open_take_syntax(open);
    CHECK(buffer&&syntax&&!xlsp_source_open_take_workspace(open));
    if(input->change_disk)diagnostic_disk(input->lib_path,"export fn value()->i64{return 1}\n");
    status=xlsp_source_open_diagnostics_params_json(open,buffer,syntax,params);
    if(input->change_disk)diagnostic_disk(input->lib_path,diagnostic_source(input->mode));
    xlsp_source_open_free(open);xlsp_source_buffer_free(buffer);xlsp_source_syntax_free(syntax);
    if(status!=XR_XIR_OK)CHECK(!*params);
    return status;
}
static XrXirStatus diagnostic_core(const DiagnosticCase *input,XrCompileResources *resources) {
    XrCompilerSession *session=NULL;XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK)return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,input->directory};
    const char *root=input->mode==6?"export fn ready()->i64{return 1}\n":root_source;
    XrModuleOverlayInput overlay[2]={{authority,"root.xr",input->root_path,root,strlen(root)},
        {authority,"lib.xr",input->lib_path,bad_source,sizeof(bad_source)-1}};
    XrXirSourceRequest request={session,input->root_path,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    const size_t included[]={1};XrXirSourceCorpusRequest corpus={&request,overlay,2,included,1};
    XrXirStatus status=input->mode==6?xr_xir_compile_source_check_corpus(&corpus,&result,&diagnostic,&failure):
        xr_xir_compile_source_check_overlay(&request,overlay,2,&result,&diagnostic,&failure);
    CHECK(!result.checked&&!result.snapshot);
    if(status==XR_XIR_BAD_STRUCTURE) {
#ifdef XR_PARSE_DIAGNOSTIC_BEFORE
        CHECK(!diagnostic.line&&!diagnostic.column&&xr_xir_compile_source_failure_empty(&failure));
        CHECK(!strcmp(diagnostic.message,input->mode==6?"source corpus inclusion failed":"module graph build failed"));
#else
        CHECK(failure.path&&!strcmp(failure.path,input->lib_path));
        CHECK(failure.origin==XR_XIR_SOURCE_DIAGNOSTIC_PARSE&&failure.has_utf16_position);
        CHECK(failure.utf16_line==0&&failure.utf16_column==13&&diagnostic.line==1&&diagnostic.column==16);
        CHECK(!strcmp(diagnostic.message,"expected variable name"));
#endif
        status=XR_XIR_OK;
    }else CHECK(status==XR_XIR_OUT_OF_MEMORY||status==XR_XIR_BUDGET);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
#ifndef XR_PARSE_DIAGNOSTIC_BEFORE
    if(status==XR_XIR_OK)CHECK(!strcmp(failure.path,input->lib_path)&&failure.utf16_column==13);
#endif
    xr_xir_compile_source_failure_free(&failure);CHECK(xr_xir_compile_source_failure_empty(&failure));return status;
}
static int diagnostic_run(const DiagnosticCase *input) {
    DiagnosticMetrics metrics={0};XrCompileResources *resources=NULL;XrJsonValue *params=NULL;
    runtime_attempts=0;runtime_fail_at=input->source_fault;
    lsp_json_calls=0;lsp_json_fail=input->json_fault;lsp_json_injected=false;
    XrCompileResourceStatus created=xr_compile_resources_new(&input->limits,&resources);
    XrXirStatus status=created==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    if(status==XR_XIR_OK)status=input->mode>=6?diagnostic_core(input,resources):diagnostic_open(input,resources,&params);
    if(resources)metrics.stats=sdk_stats(resources);
    metrics.source=runtime_attempts;metrics.json=lsp_json_calls;
    metrics.source_hit=input->source_fault!=SIZE_MAX&&runtime_attempts>input->source_fault;metrics.json_hit=lsp_json_injected;
    runtime_fail_at=SIZE_MAX;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    if(input->budget)CHECK(status==XR_XIR_BUDGET&&!params);
    else if(input->source_fault!=SIZE_MAX||input->json_fault!=SIZE_MAX)
        CHECK(status==XR_XIR_OUT_OF_MEMORY&&!params&&(metrics.source_hit||metrics.json_hit));
    else {
        CHECK(status==XR_XIR_OK);
        if(params){diagnostic_json(params,input);size_t length=0;char *json=xjson_stringify(params,&length);CHECK(json&&length);puts(json);lsp_json_free(json);}
    }
    xjson_free(params);CHECK(!lsp_json_live&&!lsp_json_bytes);
    if(input->report)fprintf(stderr,"PARSE_DIAGNOSTIC mode=%u source=%zu json=%zu source_hit=%u json_hit=%u allocated=%llu peak=%llu work=%llu producer_dead=1 physical0/0 JSON0/0\n",
        input->mode,metrics.source,metrics.json,metrics.source_hit?1u:0u,metrics.json_hit?1u:0u,
        (unsigned long long)metrics.stats.allocated_bytes,(unsigned long long)metrics.stats.peak_bytes,(unsigned long long)metrics.stats.work);
    return 0;
}
static int diagnostic_main(int argc,char **argv) {
    CHECK(argc==7||argc==8||argc==9||argc==12||argc==13);
    DiagnosticCase input={.root_uri=argv[1],.lib_uri=argv[2],.root_path=argv[3],.lib_path=argv[4],.directory=argv[5],
        .mode=(unsigned)strtoul(argv[6],NULL,10),.source_fault=SIZE_MAX,.json_fault=SIZE_MAX,
        .limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)},.report=true};
    CHECK(input.mode<=7);
    if(argc==8&&!strcmp(argv[7],"--self")){diagnostic_positions();diagnostic_occupied(&input);diagnostic_failure_lifetime(&input);return 0;}
    if(argc==8){CHECK(!strcmp(argv[7],"--change-disk")&&input.mode==2);input.change_disk=true;}
    if(argc==12) {
        CHECK(!strcmp(argv[7],"--chunk"));bool source=!strcmp(argv[8],"source");CHECK(source||!strcmp(argv[8],"json"));
        size_t begin=(size_t)strtoull(argv[9],NULL,10),count=(size_t)strtoull(argv[10],NULL,10),total=(size_t)strtoull(argv[11],NULL,10);
        CHECK(count&&begin<total&&count<=total-begin);input.report=false;
        for(size_t i=begin;i<begin+count;++i){input.source_fault=source?i:SIZE_MAX;input.json_fault=source?SIZE_MAX:i;CHECK(!diagnostic_run(&input));}
        fprintf(stderr,"PARSE_CHUNK mode=%u domain=%s begin=%zu checked=%zu total=%zu physical0/0 JSON0/0\n",input.mode,argv[8],begin,count,total);return 0;
    }
    if(argc==9){input.source_fault=(size_t)strtoull(argv[7],NULL,10);input.json_fault=(size_t)strtoull(argv[8],NULL,10);}
    if(argc==13) {
        CHECK(!strcmp(argv[7],"--limits"));input.budget=strtoul(argv[8],NULL,10)!=0;
        input.limits=(XrCompileResourceLimits){strtoull(argv[9],NULL,10),strtoull(argv[10],NULL,10),strtoull(argv[11],NULL,10)};
        CHECK(!strcmp(argv[12],"exact"));
    }
    return diagnostic_run(&input);
}
#ifdef XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=diagnostic_main(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
#else
int main(int argc,char **argv){return diagnostic_main(argc,argv);}
#endif
