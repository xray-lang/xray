/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_source_syntax.h"
#include "module/xmodule_fingerprint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while(0)
#include "xir_sdk_resource_test.h"
static const char argument_text[]="call( /*,*/ ref value,\n (other), \"\xf0\x9f\x98\x80\", inner(1, 2), ...items, )\n";
static const XrLspRange outer_ranges[]={{{0,12},{0,21}},{{1,1},{1,8}},{{1,10},{1,14}},{{1,16},{1,27}},{{1,29},{1,37}}};
static const char *argument_program(unsigned variant) {
    const char *programs[]={argument_text,"fn empty(){}\n","call(1,2) = 3\n",
        "call(1,2)\nvar *** = ;\ncall(3,4)\n","call(_)\n","call(\"\"\"\r\n  alpha\r\n  \"\"\"\r\n ,2)\r\n","call(1,2)\ncall(3,"};
    CHECK(variant<7);return programs[variant];
}
static bool same_position(XrLspPosition a,XrLspPosition b){return a.line==b.line&&a.character==b.character;}
static void argument_facts(const XlspSourceCallArguments *owner,unsigned variant) {
    const XlspSourceCallArgumentsView *view=xlsp_source_call_arguments_view(owner);CHECK(view);
    size_t expected=variant==0||variant==3?2:variant>=4?1:0;
    if(view->count!=expected)fprintf(stderr,"ARGUMENT_REACHABILITY expected=%zu actual=%zu\n",expected,view->count);
    CHECK(view->count==expected);CHECK(!strcmp(view->uri,"file:///arguments.xr")&&view->version==11);
    if(variant==1||variant==2){CHECK(!view->calls);return;}
    for(size_t i=0;i<view->count;++i) {
        const XlspSourceArgumentCall *call=&view->calls[i];CHECK(call->call.has_callee);
        if(variant==3||variant==6) {
            CHECK((call->call.callee.start.line==0||call->call.callee.start.line==2)&&
                call->call.callee.start.character==0&&call->call.argument_count==2);
            CHECK(call->arguments[0].range.start.character==5&&call->arguments[1].range.start.character==7);continue;
        }
        if(variant==4){CHECK(call->call.argument_count==1&&!call->arguments[0].has_variable);continue;}
        if(variant==5) {
            CHECK(call->call.argument_count==2&&call->arguments[0].range.start.line==0&&call->arguments[0].range.start.character==5);
            CHECK(call->arguments[0].range.end.line==2&&call->arguments[0].range.end.character==5);
            CHECK(call->arguments[1].range.start.line==3&&call->arguments[1].range.start.character==2);continue;
        }
        if(call->call.callee.start.line==0) {
            CHECK(call->call.callee.start.character==0&&call->call.argument_count==5);
            for(size_t p=0;p<5;++p) {
                CHECK(same_position(call->arguments[p].range.start,outer_ranges[p].start));
                CHECK(same_position(call->arguments[p].range.end,outer_ranges[p].end));
                CHECK(call->arguments[p].has_variable==(p==0));
            }
            CHECK(call->arguments[0].variable.start.line==0&&call->arguments[0].variable.start.character==16);
            CHECK(call->arguments[0].variable.end.character==21);
        } else {
            CHECK(call->call.callee.start.line==1&&call->call.callee.start.character==16&&call->call.argument_count==2);
            CHECK(call->arguments[0].range.start.character==22&&call->arguments[0].range.end.character==23);
            CHECK(call->arguments[1].range.start.character==25&&call->arguments[1].range.end.character==26);
        }
    }
}
static XlspSyntaxSnapshot *argument_producer(XrCompileResources *resources,unsigned variant) {
    const char *program=argument_program(variant);
    size_t length=strlen(program);char *text=xr_malloc(length+1);CHECK(text);memcpy(text,program,length+1);
    char uri[]="file:///arguments.xr";XlspSourceDocument input={uri,text,sizeof(uri)-1,length,11};
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    XlspSyntaxSnapshot *syntax=NULL;XrParseStatus status=xlsp_source_syntax_build(session,&input,&syntax);
    CHECK(status==(variant==2||variant==3||variant==6?XR_PARSE_RECOVERED:XR_PARSE_OK));
    xr_compile_session_free(session);memset(uri,'?',sizeof(uri));memset(text,'?',length);xr_free(text);return syntax;
}
static size_t argument_attachment(XrCompileResources *resources,const XlspSyntaxSnapshot *syntax,bool failures) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        if(probe&&!failures)break;
        size_t blocks=runtime_live,bytes=runtime_bytes,start=runtime_attempts;uint64_t live=sdk_stats(resources).live_bytes;
        XlspSourceCallArguments *owner=NULL;runtime_fail_at=probe?start+probe-1:SIZE_MAX;
        XrXirStatus status=xlsp_source_syntax_arguments(syntax,&owner);runtime_fail_at=SIZE_MAX;
        if(!probe){CHECK(status==XR_XIR_OK);sites=runtime_attempts-start;CHECK(sites);argument_facts(owner,0);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&!owner);
        xlsp_source_call_arguments_free(owner);CHECK(runtime_live==blocks&&runtime_bytes==bytes&&sdk_stats(resources).live_bytes==live);
        if(probe){CHECK(xlsp_source_syntax_arguments(syntax,&owner)==XR_XIR_OK);argument_facts(owner,0);xlsp_source_call_arguments_free(owner);}
    }
    return sites;
}
static XrXirStatus argument_once(unsigned variant,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *syntax=NULL;XlspSourceCallArguments *owner=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);XrXirStatus status=XR_XIR_OK;
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    const char *program=argument_program(variant);XlspSourceDocument input={"file:///arguments.xr",program,20,strlen(program),11};
    XrParseStatus parsed=xlsp_source_syntax_build(session,&input,&syntax);
    if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED) {
        status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;
    }
    CHECK(parsed==(variant==2||variant==3||variant==6?XR_PARSE_RECOVERED:XR_PARSE_OK));
    status=xlsp_source_syntax_arguments(syntax,&owner);if(status==XR_XIR_OK&&stats)*stats=sdk_stats(resources);
done:
    xlsp_source_syntax_free(syntax);xr_compile_session_free(session);xr_compile_resources_release(resources);
    if(status==XR_XIR_OK)argument_facts(owner,variant);
    xlsp_source_call_arguments_free(owner);CHECK(!runtime_live&&!runtime_bytes);return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));bool failures=argc==2;size_t sites=0;
    CHECK(!xlsp_source_call_arguments_view(NULL));xlsp_source_call_arguments_free(NULL);
    const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    for(unsigned variant=0;variant<7;++variant) {
        XrCompileResources *resources=sdk_ledger(&sdk_unlimited);XlspSyntaxSnapshot *syntax=argument_producer(resources,variant);
        if(!variant)sites=argument_attachment(resources,syntax,failures);
        XlspSourceCallArguments *owner=NULL;CHECK(xlsp_source_syntax_arguments(syntax,&owner)==XR_XIR_OK);
        if(!variant) {
            XrFingerprint fingerprint={{0}};xr_module_source_fingerprint(argument_text,&fingerprint);
            CHECK(!memcmp(&fingerprint,&xlsp_source_call_arguments_view(owner)->fingerprint,sizeof(fingerprint)));
        }
        CHECK(xlsp_source_syntax_arguments(syntax,&owner)==XR_XIR_BAD_STRUCTURE);
        xlsp_source_syntax_free(syntax);xr_compile_resources_release(resources);argument_facts(owner,variant);
        xlsp_source_call_arguments_free(owner);CHECK(!runtime_live&&!runtime_bytes);
        XrCompileResourceStats required={0};runtime_attempts=0;CHECK(argument_once(variant,limits,&required)==XR_XIR_OK);size_t prefixes=runtime_attempts;
        printf("Argument producer variant=%u prefixes=%zu allocated=%llu peak=%llu work=%llu\n",variant,prefixes,
            (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
        if(failures) {
            for(size_t p=0;p<prefixes;++p){runtime_attempts=0;runtime_fail_at=p;CHECK(argument_once(variant,limits,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>p);}
            runtime_fail_at=SIZE_MAX;
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
                XrCompileResourceLimits cut={required.allocated_bytes,required.peak_bytes,required.work};
                uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;CHECK(*bound&&*bound<INT64_MAX);
                *bound=(uint64_t)((int64_t)*bound+delta);CHECK(argument_once(variant,cut,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
            }
        }
    }
    printf("Argument syntax sites=%zu full_FI=%u exact_tokens=1 unicode=1 recovery=1 producer_dead=1 physical0/0\n",sites,failures?1u:0u);return 0;
}
