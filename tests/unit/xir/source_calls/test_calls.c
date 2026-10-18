/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_calls.c - Canonical calls retain their actual source body after producer death
 */
#include "app/lsp/xlsp_source_bindings.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
static const char *const texts[]={
    "fn target(v:i64)->i64{return v}\nfn wrapper(v:i64)->i64 {\n return target(v)+target(v)\n}\nconst answer=wrapper(1)\n",
    "fn target(v:i64)->i64{return v}\nfn apply(cb:fn(i64)->i64,v:i64)->i64 {return cb(v)}\nconst answer=apply(target,1)\n",
    "fn target(v:i64)->i64{return v}\nfn outer()->fn(i64)->i64 {return fn(v:i64)->i64 {return target(v)}}\nconst cb=outer()\nconst answer=cb(1)\n",
    "import {visible as alias} from \"./lib\"\nimport \"./lib\" as lib\nfn wrapper(v:i64)->i64 {return alias(v)}\nconst answer=lib.visible(wrapper(1))\n",
    "fn target(v:i64)->i64{return v}\nstruct S {\n value:i64\n static pick(v:i64)->i64 {return target(v)}\n}\nconst answer=S.pick(1)\n",
    "final class C{x:Array<i64> constructor(){this.x=[1]} bad(){this.x[0]=2}}"
};
static bool same_range(XrXirSourceRange a,XrXirSourceRange b) {
    return a.module==b.module&&a.line==b.line&&a.column==b.column&&a.end_line==b.end_line&&a.end_column==b.end_column;
}
static void inspect(const XrXirSourceView *view,const XrXirSourceSyntaxView *syntax,const XrXirSourceCalls *calls,unsigned mode) {
    CHECK(view&&view->complete&&syntax&&calls&&calls->declaration_count==view->declaration_count);
    unsigned direct=0,indirect=0,initializers=0,wrapper=0,closure=0,methods=0,missing=0;
    for(uint32_t i=0;i<calls->count;++i) {
        const XrXirSourceCall *call=&calls->items[i];
        if(!call->selection_known) {
            CHECK(mode==5&&!call->range.line&&!call->range.column&&!call->range.end_line&&!call->range.end_column);++missing;
        } else CHECK(call->range.line>0&&call->range.column>0&&call->range.end_line==call->range.line&&call->range.end_column>call->range.column);
        if(call->reference==UINT32_MAX)++indirect;
        else {
            CHECK(call->reference<view->reference_count&&view->references[call->reference].access==XR_XIR_SOURCE_CALL);
            if(call->selection_known)CHECK(same_range(call->range,syntax->references[call->reference]));++direct;
        }
        if(call->module_initializer){CHECK(!call->caller);++initializers;continue;}
        CHECK(call->caller&&call->caller<=view->declaration_count);
        const XrXirSourceDeclaration *caller=&view->declarations[call->caller-1];
        const XrXirSourceFunctionRange *body=&calls->functions[call->caller-1];
        CHECK(body->known&&body->range.module==call->range.module);
        if(call->selection_known)CHECK(body->range.line<=call->range.line&&body->range.end_line>=call->range.line);
        if(!strcmp(caller->name,"wrapper")) {
            ++wrapper;
            if(mode==0)CHECK(body->range.line==2&&body->range.end_line==4&&body->range.end_column==2&&call->range.line==3);
        }
        if(syntax->declarations[call->caller-1].role==XR_XIR_SOURCE_SYNTAX_CLOSURE) {
            CHECK(caller->parent&& !strcmp(view->declarations[caller->parent-1].name,"outer"));++closure;
        }
        if(syntax->declarations[call->caller-1].role==XR_XIR_SOURCE_SYNTAX_METHOD)++methods;
    }
    static const unsigned expected_direct[]={3,1,2,3,2,1},expected_indirect[]={0,1,1,0,0,0},expected_init[]={1,1,2,2,1,0};
    CHECK(direct==expected_direct[mode]&&indirect==expected_indirect[mode]&&initializers==expected_init[mode]);
    CHECK(wrapper==(mode==0?2u:mode==3?1u:0u)&&closure==(mode==2?1u:0u)&&methods==((mode==4||mode==5)?1u:0u)&&missing==(mode==5?1u:0u));
    printf("Owned source calls mode=%u direct=%u indirect=%u initializer=%u wrapper=%u closure=%u methods=%u\n",mode,direct,indirect,initializers,wrapper,closure,methods);
}
static int run(int argc,char **argv) {
    CHECK(argc==3);unsigned mode=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<6);(void)sdk_unlimited;
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=sdk_ledger(&limits);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    size_t length=strlen(texts[mode]),uri_length=strlen(argv[1]);
    char *text=sdk_fixture_malloc(length+1),*uri=sdk_fixture_malloc(uri_length+1);CHECK(text&&uri);
    memcpy(text,texts[mode],length+1);memcpy(uri,argv[1],uri_length+1);
    XlspSourceDocument input={uri,text,uri_length,length,1};XlspSourceBindings *owner=NULL;
    XrXirStatus status=xlsp_source_bindings_build(&context,&input,1,&owner);
    if(status!=XR_XIR_OK)fprintf(stderr,"Source calls mode=%u status=%d\n",mode,status);
    CHECK(status==XR_XIR_OK&&owner);XrCompileResourceStats stats=sdk_stats(resources);
    memset(text,'?',length);memset(uri,'?',uri_length);sdk_fixture_free(text);sdk_fixture_free(uri);xr_compile_resources_release(resources);
    inspect(xlsp_source_bindings_view(owner,0),xlsp_source_bindings_syntax(owner,0),xlsp_source_bindings_calls(owner,0),mode);
    const XrXirSourceTypeFacts *facts=xlsp_source_bindings_types(owner,0);CHECK(facts&&facts->declaration_count==xlsp_source_bindings_view(owner,0)->declaration_count);
    if(mode==4||mode==5){unsigned found=0;for(uint32_t i=0;i<facts->declaration_count;++i)if(facts->declarations[i].kind==(mode==4?XR_XIR_SOURCE_TYPE_STRUCT:XR_XIR_SOURCE_TYPE_CLASS)){CHECK(facts->declarations[i].range_known);++found;}CHECK(found==1);}
    xlsp_source_bindings_free(owner);CHECK(!runtime_live&&!runtime_bytes);
    printf("Source calls resources allocated=%llu peak=%llu work=%llu physical=0/0\n",(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
    return 0;
}
int wmain(int argc,wchar_t **wide) {
    XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);
    int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;
}
