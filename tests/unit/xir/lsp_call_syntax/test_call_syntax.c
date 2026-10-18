/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_call_syntax.c - Exact parser separators survive producer death
 */
#include "app/lsp/xlsp_source_syntax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
static const char source_text[]=
    "fn host() {\n"
    " call(\n"
    "  nested(\"a,b\", 2),\n"
    "  generic<fn(i64,i64)->i64>(1),\n"
    "  Array<i64>(0),\n"
    "  \"x,y\",\n"
    "  5,\n"
    " )\n"
    " obj.method<i64>(1, 2)\n"
    " call(/* comment , ) */ 1, \"\xf0\x9f\x98\x80,x\", 3)\n"
    " call(/a,b/, 2)\n"
    "}\n";
typedef struct CallCase {uint32_t row,column;size_t active,count;uint32_t name_row,name_column,name_end;} CallCase;
/* Literal UTF-16 coordinates, independent of parser-produced locations. */
static const CallCase cases[]={
    {1,6,0,5,1,1,5},{2,12,0,2,2,2,8},{2,15,1,2,2,2,8},
    {2,18,0,5,1,1,5},{2,19,1,5,1,1,5},{3,29,0,1,3,2,9},
    {4,14,0,1,4,2,7},{4,15,2,5,1,1,5},{4,16,3,5,1,1,5},
    {5,5,3,5,1,1,5},{5,8,4,5,1,1,5},{6,4,5,5,1,1,5},
    {7,1,5,5,1,1,5},{8,19,1,2,8,5,11},
    {9,18,0,3,9,1,5},{9,30,1,3,9,1,5},{9,34,2,3,9,1,5},
    {10,9,0,2,10,1,5},{10,12,1,2,10,1,5}
};
typedef struct OtherCase {
    const char *text;
    XrParseStatus parsed;
    XrLspPosition position;
    XrXirStatus status;
    size_t active,count;
    bool has_callee;
} OtherCase;
static const OtherCase other_cases[]={
    {"call(1,2)\nvar *** = ;\ncall(3,4)\n",XR_PARSE_RECOVERED,{2,7},XR_XIR_OK,1,2,true},
    {"call(1,2)\ncall(3,",XR_PARSE_RECOVERED,{0,7},XR_XIR_OK,1,2,true},
    {"call(1,2)\ncall(3,",XR_PARSE_RECOVERED,{1,7},XR_XIR_UNRESOLVED,0,0,false},
    {"call(r\"a,b\", 2)\n",XR_PARSE_OK,{0,9},XR_XIR_OK,0,2,true},
    {"call(r\"a,b\", 2)\n",XR_PARSE_OK,{0,12},XR_XIR_OK,1,2,true},
    {"Map<String,i64>()\n",XR_PARSE_OK,{0,16},XR_XIR_OK,0,0,true},
    {"(factory())(1,2)\n",XR_PARSE_OK,{0,14},XR_XIR_OK,1,2,false},
    {"call(1,\r\n 2)\r\n",XR_PARSE_OK,{1,1},XR_XIR_OK,1,2,true},
    {"call(1,\n fn(a:i64,b:i64)->i64 { return a+b },\n 3)\n",XR_PARSE_OK,{2,1},XR_XIR_OK,2,3,true},
    {"call(1,2) = 3\ncall(4,5)\n",XR_PARSE_RECOVERED,{0,7},XR_XIR_UNRESOLVED,0,0,false}
};
static XrXirStatus other_facts(XlspSyntaxSnapshot *snapshot,const OtherCase *c) {
    XlspSourceCallSyntax result={0};result.active_argument=73;result.argument_count=91;
    XrXirStatus status=xlsp_source_syntax_call(snapshot,c->position,&result);
    if(status==XR_XIR_BUDGET||status==XR_XIR_OUT_OF_MEMORY)return status;
    CHECK(status==c->status);
    if(status==XR_XIR_OK)CHECK(result.active_argument==c->active&&result.argument_count==c->count&&
        result.has_callee==c->has_callee);
    else CHECK(result.active_argument==73&&result.argument_count==91);
    return XR_XIR_OK;
}
static XrXirStatus facts(XlspSyntaxSnapshot *snapshot) {
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i) {
        const CallCase *c=&cases[i];XlspSourceCallSyntax result={0};
        XrXirStatus status=xlsp_source_syntax_call(snapshot,(XrLspPosition){c->row,c->column},&result);
        if(status!=XR_XIR_OK)return status;
        if(result.active_argument!=c->active||result.argument_count!=c->count||!result.has_callee||
            result.callee.start.line!=c->name_row||result.callee.start.character!=c->name_column||
            result.callee.end.line!=c->name_row||result.callee.end.character!=c->name_end) {
            fprintf(stderr,"case %zu: active=%zu count=%zu callee=%u:%u..%u:%u\n",i,
                result.active_argument,result.argument_count,result.callee.start.line,result.callee.start.character,
                result.callee.end.line,result.callee.end.character);CHECK(false);
        }
    }
    XlspSourceCallSyntax result={0};result.active_argument=73;result.argument_count=91;
    XrXirStatus status=xlsp_source_syntax_call(snapshot,(XrLspPosition){7,2},&result);
    if(status==XR_XIR_BUDGET||status==XR_XIR_OUT_OF_MEMORY)return status;
    CHECK(status==XR_XIR_UNRESOLVED&&result.active_argument==73&&result.argument_count==91);
    status=xlsp_source_syntax_call(snapshot,(XrLspPosition){9,29},&result);
    if(status==XR_XIR_BUDGET||status==XR_XIR_OUT_OF_MEMORY)return status;
    CHECK(status==XR_XIR_BAD_STRUCTURE&&result.active_argument==73&&result.argument_count==91);
    return XR_XIR_OK;
}
static XrXirStatus once(size_t mode,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *snapshot=NULL;
    XrCompileResourceStatus made=XR_COMPILE_RESOURCE_OK;
    if(runtime_fail_at==SIZE_MAX)resources=sdk_ledger(&limits);
    else made=xr_compile_resources_new(&limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirStatus status=XR_XIR_OK;bool released=false;
    XrCompilerSessionStatus opened=xr_compile_session_new(resources,&session);
    if(opened!=XR_COMPILER_SESSION_OK){status=opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;goto done;}
    const OtherCase *other=mode?&other_cases[mode-1]:NULL;
    const char *source=other?other->text:source_text;
    char uri[]="file:///call.xr";size_t length=strlen(source);
    char *text=xr_malloc(length+1);CHECK(text);memcpy(text,source,length+1);
    XlspSourceDocument input={uri,text,sizeof(uri)-1,length,17};
    XrParseStatus parsed=xlsp_source_syntax_build(session,&input,&snapshot);
    memset(text,'?',length+1);xr_free(text);text=NULL;memset(uri,'?',sizeof(uri));memset(&input,0,sizeof(input));
    if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED) {
        CHECK(!snapshot);
        if(parsed!=XR_PARSE_BUDGET&&parsed!=XR_PARSE_OUT_OF_MEMORY)fprintf(stderr,"parse status %d\n",(int)parsed);
        status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
        goto done;
    }
    CHECK(parsed==(other?other->parsed:XR_PARSE_OK));
    xr_compile_session_free(session);session=NULL;
    xr_compile_resources_release(resources);released=true;
    status=other?other_facts(snapshot,other):facts(snapshot);
    if(status==XR_XIR_OK&&stats)*stats=sdk_stats(resources);
done:
    xlsp_source_syntax_free(snapshot);xr_compile_session_free(session);
    if(!released)xr_compile_resources_release(resources);
    CHECK(!runtime_live&&!runtime_bytes);return status;
}
int main(int argc,char **argv) {
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    CHECK(limits.work<sdk_unlimited.work);size_t total=0;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));
    for(size_t mode=0;mode<=sizeof(other_cases)/sizeof(*other_cases);++mode) {
    XrCompileResourceStats required={0};
    runtime_attempts=0;CHECK(once(mode,limits,&required)==XR_XIR_OK);size_t sites=runtime_attempts;total+=sites;
    printf("call syntax mode=%zu sites=%zu allocated=%llu peak=%llu work=%llu; physical=0/0\n",mode,sites,
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
    if(argc==2&&!strcmp(argv[1],"--resources")) {
        for(size_t i=0;i<sites;++i) {
            runtime_attempts=0;runtime_fail_at=i;
            CHECK(once(mode,limits,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);
        }
        runtime_fail_at=SIZE_MAX;
        for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits cut={required.allocated_bytes,required.peak_bytes,required.work};
            uint64_t *bound=axis==0?&cut.allocated_bytes:axis==1?&cut.live_bytes:&cut.work;
            CHECK(*bound>0&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
            CHECK(once(mode,cut,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
        printf("all %zu allocation prefixes and 9 budget cuts passed; physical=0/0\n",sites);
    }
    }
    printf("call syntax total allocation sites=%zu; modes=%zu; physical=0/0\n",total,
        1+sizeof(other_cases)/sizeof(*other_cases));
    return 0;
}
