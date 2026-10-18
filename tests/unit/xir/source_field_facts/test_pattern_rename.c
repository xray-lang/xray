/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_rename.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
static const char *const source[]={
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value}, E.B {value}->value }\n",
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:value}, E.B {value}->value }\n",
    "enum E { A { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:value}->value }\n"
};
static const char *const expected[]={
    "enum E { A { payload:i64 }, B { value:i64 } }\nvar x=E.A {payload:1}\nvar y=match(x){ E.A {payload: value}, E.B {value}->value }\n",
    "enum E { A { value:i64 }, B { payload:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value}, E.B {payload: value}->value }\n",
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value: local\xf0\x9f\x98\x80}, E.B {value: local\xf0\x9f\x98\x80}->local\xf0\x9f\x98\x80 }\n",
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:local}, E.B {value: local}->local }\n",
    "enum E { A { payload:i64 } }\nvar x=E.A {payload:1}\nvar y=match(x){ E.A {payload:value}->value }\n",
    "enum E { A { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:local}->local }\n"
};
static size_t offset(const char *text,int64_t line,int64_t column) {
    CHECK(line>=0&&column>=0);size_t at=0;while(line){CHECK(text[at]);if(text[at++]=='\n')--line;}
    while(column--){CHECK(text[at]&&text[at]!='\n');++at;}return at;
}
static XrLspPosition selection(const char *text,unsigned occurrence) {
    const char *found=text;while(occurrence--){found=strstr(found,"value");CHECK(found);if(occurrence)++found;}
    XrLspPosition position={0};for(const char *p=text;p<found;++p){if(*p=='\n'){++position.line;position.character=0;}else ++position.character;}
    return position;
}
static XrXirStatus once(const char *uri,unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t json_fault,bool *injected) {
    XrCompileResources *resources=NULL;XrCompileResourceStatus created=xr_compile_resources_new(&caps,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    unsigned program=mode<3?0:mode==3?1:2,nth=mode==0||mode==4?1:mode==1?2:mode==2?6:mode==3?7:5;
    const char *name=mode<2||mode==4?"payload":mode==2?"local\xf0\x9f\x98\x80":"local";
    char input[1024];strcpy(input,source[program]);
    XlspSourceDocument document={uri,input,strlen(uri),strlen(input),9};
    XlspSourceRenameInput rename={0,selection(input,nth),name,strlen(name),false,NULL};
    lsp_json_calls=0;lsp_json_fail=json_fault;lsp_json_injected=false;XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_rename_json(&context,&document,1,&rename,&result);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    if(stats)*stats=sdk_stats(resources);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
    CHECK(!strcmp(input,source[program]));
    if(status==XR_XIR_OK){
        XrJsonValue *changes=xjson_get_array(result,"documentChanges");CHECK(changes&&changes->as.array.count==1);
        XrJsonValue *change=xjson_array_get(changes,0),*identity=xjson_get_object(change,"textDocument"),*edits=xjson_get_array(change,"edits");
        CHECK(identity&&!strcmp(xjson_get_string(identity,"uri"),uri)&&xjson_get_int(identity,"version")==9);
        CHECK(edits&&edits->as.array.count==(mode==1||mode==5?2:3));
        char actual[2048];strcpy(actual,input);
        for(int i=0;i<edits->as.array.count;++i){
            XrJsonValue *edit=xjson_array_get(edits,i),*range=xjson_get_object(edit,"range"),*start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");
            const char *replacement=xjson_get_string(edit,"newText");CHECK(start&&end&&replacement);
            size_t begin=offset(input,xjson_get_int(start,"line"),xjson_get_int(start,"character"));
            size_t finish=offset(input,xjson_get_int(end,"line"),xjson_get_int(end,"character"));
            size_t total=strlen(actual),length=strlen(replacement);CHECK(begin<=finish&&finish<=total&&total-(finish-begin)+length<sizeof(actual));
            memmove(actual+begin+length,actual+finish,total-finish+1);memcpy(actual+begin,replacement,length);
        }
        if(strcmp(actual,expected[mode]))fprintf(stderr,"mode=%u actual=%s expected=%s\n",mode,actual,expected[mode]);CHECK(!strcmp(actual,expected[mode]));
    }else CHECK(!result);
    xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static int run(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==3||argc==4);unsigned mode=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<6);bool full=argc==4;
    if(full)CHECK(!strcmp(argv[3],"--full"));
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    CHECK(once(argv[1],mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;
    if(full){
        for(size_t i=0;i<sites;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(once(argv[1],mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}runtime_fail_at=SIZE_MAX;
        for(size_t i=0;i<json_sites;++i){bool injected=false;CHECK(once(argv[1],mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
    }
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){
        XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;
        CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(once(argv[1],mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    printf("Pattern rename mode=%u SourceFI=%zu JSONFI=%zu full=%u allocated=%llu peak=%llu work=%llu physical0/0\n",mode,sites,json_sites,full?1u:0u,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);return 0;
}
#if XR_OS_WINDOWS
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
