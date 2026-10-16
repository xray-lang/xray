/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_inlay.h"
#include "app/lsp/xlsp_source_workspace.h"
#include "shared/xr_param_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"
#include "app/lsp/xlsp_source_workspace.c"
#include "parameter_hint_cases.h"
static XrLspRange parameter_range(const char *program) {
    uint32_t lines=0;for(const char *p=program;*p;++p)if(*p=='\n')++lines;
    return (XrLspRange){{0,0},{lines,0}};
}
static XlspSourceCallArguments *parameter_syntax(XrCompileResources *resources,const char *uri,const char *program,int64_t version) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XlspSourceDocument document={uri,program,strlen(uri),strlen(program),version};XlspSyntaxSnapshot *syntax=NULL;
    CHECK(xlsp_source_syntax_build(session,&document,&syntax)==XR_PARSE_OK);
    XlspSourceCallArguments *arguments=NULL;CHECK(xlsp_source_syntax_arguments(syntax,&arguments)==XR_XIR_OK);
    xlsp_source_syntax_free(syntax);xr_compile_session_free(session);return arguments;
}
static XlspSourceSnapshot *parameter_producer(XrCompileResources *resources,const char *uri,const char *program,
    XlspSourceCallArguments **arguments) {
    size_t length=strlen(program),uri_length=strlen(uri);char *text=xr_malloc(length+1),*name=xr_malloc(uri_length+1);CHECK(text&&name);
    memcpy(text,program,length+1);memcpy(name,uri,uri_length+1);XlspSourceDocument input={name,text,uri_length,length,9};
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XlspSourceWorkspace *workspace=NULL;
    unsigned stage=0;XlspSourceWorkspaceFailure failure={0};
    XrXirStatus status=xlsp_source_workspace_build(&context,&input,1,&workspace,&stage,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"parameter stage=%u status=%u %s\n%s",stage,status,failure.diagnostic.message,program);
    CHECK(status==XR_XIR_OK);*arguments=parameter_syntax(resources,name,text,9);
    XlspSourceSnapshot *snapshot=workspace->snapshots[0];workspace->snapshots[0]=NULL;
    xlsp_source_workspace_free(workspace);xlsp_source_workspace_failure_free(&failure);
    memset(text,'?',length);memset(name,'?',uri_length);xr_free(text);xr_free(name);return snapshot;
}
static void parameter_check(const XlspSourceParameterHints *hints,const ParameterHintCase *test) {
    if(hints->count!=test->count)fprintf(stderr,"Parameter hint count expected=%zu actual=%zu\n%s",test->count,hints->count,test->source);
    CHECK(hints->count==test->count);
    for(size_t i=0;i<hints->count;++i) {
        const XlspSourceParameterHint *hint=&hints->items[i];const ParameterHintExpected *expected=&test->hints[i];
        CHECK(!strcmp(hint->label.text,expected->label)&&hint->label.length==strlen(expected->label));
        CHECK(hint->mode==expected->mode&&hint->target_native_identity==expected->native_identity&&hint->target_kind==expected->kind);
        CHECK(hint->target_identity.text&&hint->target_identity.length&&hint->target_range.line>0&&hint->target_range.column>0);
        CHECK(!hint->target_range.module&&hint->target_range.end_line==hint->target_range.line&&hint->target_range.end_column>hint->target_range.column);
        if(expected->kind==XR_XIR_SOURCE_INTRINSIC) {
            CHECK(!hint->target_path.text&&!hint->target_path.length);
            CHECK(strstr(hint->target_identity.text,"xray-core-assertions-v1"));
        } else CHECK(hint->target_path.text&&hint->target_path.length);
        if(i)CHECK(hints->items[i-1].position.line<hint->position.line||
            (hints->items[i-1].position.line==hint->position.line&&hints->items[i-1].position.character<hint->position.character));
    }
}
static size_t parameter_attachment(XrCompileResources *resources,const XlspSourceSnapshot *snapshot,
    const XlspSourceCallArguments *arguments,const ParameterHintCase *test,bool failures) {
    size_t sites=0;XrLspRange range=parameter_range(test->source);
    for(size_t probe=0;probe<=sites;++probe) {
        if(probe&&!failures)break;
        size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;uint64_t live=sdk_stats(resources).live_bytes;
        XlspSourceParameterHints hints={0};runtime_fail_at=probe?start+probe-1:SIZE_MAX;
        XrXirStatus status=xlsp_source_parameter_hints(snapshot,arguments,range,&hints);runtime_fail_at=SIZE_MAX;
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-start;CHECK(sites);parameter_check(&hints,test);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&!hints.items&&!hints.count);
        xlsp_source_parameter_hints_free(&hints);CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(resources).live_bytes==live);
    }
    return sites;
}
static void parameter_stale(XrCompileResources *resources,const XlspSourceSnapshot *snapshot,const char *uri,const char *program) {
    const char *other_uri="file:///different.xr";char *different=xr_malloc(strlen(program)+1);CHECK(different);strcpy(different,program);
    char *number=strstr(different,"left=1");CHECK(number);number[5]='9';
    for(unsigned mode=0;mode<4;++mode) {
        XrCompileResources *foreign=mode==3?sdk_ledger(&sdk_unlimited):NULL;
        XlspSourceCallArguments *arguments=parameter_syntax(foreign?foreign:resources,mode==0?other_uri:uri,
            mode==2?different:program,mode==1?10:9);
        XlspSourceParameterHints hints={0};size_t blocks=runtime_live,bytes=runtime_bytes;uint64_t live=sdk_stats(resources).live_bytes;
        XrXirStatus status=xlsp_source_parameter_hints(snapshot,arguments,parameter_range(program),&hints);
        CHECK(status==(mode==0?XR_XIR_UNRESOLVED:XR_XIR_BAD_STRUCTURE)&&!hints.items&&!hints.count);
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(resources).live_bytes==live);
        xlsp_source_call_arguments_free(arguments);xr_compile_resources_release(foreign);
    }
    memset(different,'?',strlen(program));xr_free(different);
}
static void parameter_regions(const XlspSourceSnapshot *snapshot,const XlspSourceCallArguments *arguments) {
    XlspSourceParameterHints hints={0};XrLspRange empty={{3,18},{3,18}};
    CHECK(xlsp_source_parameter_hints(snapshot,arguments,empty,&hints)==XR_XIR_OK&&!hints.count&&!hints.items);
    XrLspRange selected={{3,18},{3,19}};
    CHECK(xlsp_source_parameter_hints(snapshot,arguments,selected,&hints)==XR_XIR_OK&&hints.count==1);
    CHECK(hints.items[0].position.line==3&&hints.items[0].position.character==18);xlsp_source_parameter_hints_free(&hints);
    selected.end.character=99;CHECK(xlsp_source_parameter_hints(snapshot,arguments,selected,&hints)==XR_XIR_BAD_STRUCTURE&&!hints.items);
}
#include "parameter_hint_provenance.h"
int main(int argc,char **argv) {
    CHECK(argc==3||(argc==4&&!strcmp(argv[3],"--resources")));bool failures=argc==4;size_t sites=0;
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    CHECK(limits.work<sdk_unlimited.work);
    for(size_t c=0;c<sizeof(parameter_cases)/sizeof(*parameter_cases);++c) {
        const ParameterHintCase *test=&parameter_cases[c];XrCompileResources *resources=sdk_ledger(&limits);
        XlspSourceCallArguments *arguments=NULL;XlspSourceSnapshot *snapshot=parameter_producer(resources,argv[1],test->source,&arguments);
        XlspSourceParameterHints hints={0};XrXirStatus status=xlsp_source_parameter_hints(snapshot,arguments,parameter_range(test->source),&hints);
        if(status!=(test->unavailable?XR_XIR_UNSUPPORTED:XR_XIR_OK))fprintf(stderr,"parameter case=%zu status=%u\n",c,status);
        CHECK(status==(test->unavailable?XR_XIR_UNSUPPORTED:XR_XIR_OK));
        if(!test->unavailable)parameter_check(&hints,test);else CHECK(!hints.items&&!hints.count);
        if(c==0){parameter_stale(resources,snapshot,argv[1],test->source);parameter_regions(snapshot,arguments);}
        if(c==0||c==3||c==5)sites+=parameter_attachment(resources,snapshot,arguments,test,failures);
        if(c==1)CHECK(hints.items[0].position.line==3&&hints.items[0].position.character==11);
        if(c==11)CHECK(hints.items[0].position.character==15&&hints.items[1].position.character==20);
        xlsp_source_call_arguments_free(arguments);xlsp_source_snapshot_free(snapshot);xr_compile_resources_release(resources);
        if(!test->unavailable)parameter_check(&hints,test);xlsp_source_parameter_hints_free(&hints);CHECK(!runtime_live&&!runtime_bytes);
    }
    parameter_crossroots(argv[1],argv[2],&limits);sites+=parameter_long(argv[1],&limits,failures);
    printf("Parameter hints cases=16 sites=%zu full_FI=%u stale=4 ref=1 variadic=1 core=1 unicode=1 cross_root=2 changed_provenance=1 native_shadow=1 long=97 producer_dead=1 physical0/0\n",sites,failures?1u:0u);return 0;
}
