/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_selection.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"

static const char *const texts[]={"A\xf0\x9f\x98\x80Word\r\n \tfoo_2\n","","word","last\xf0\x9f\x98\x80"};
static const char *const positions[]={"[{\"line\":0,\"character\":4},{\"line\":1,\"character\":4},{\"line\":2,\"character\":0}]",
    "[{\"line\":0,\"character\":0}]","[{\"line\":0,\"character\":2}]","[{\"line\":0,\"character\":6}]"};
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static XrJsonValue *range_fact(XrJsonValue *node,int begin_line,int begin,int end_line,int end) {
    CHECK(node&&node->type==XR_JSON_OBJECT);XrJsonValue *range=xjson_get_object(node,"range");CHECK(range);
    XrJsonValue *start=xjson_get_object(range,"start"),*finish=xjson_get_object(range,"end");CHECK(start&&finish);
    CHECK(xjson_get_int(start,"line")==begin_line&&xjson_get_int(start,"character")==begin&&
        xjson_get_int(finish,"line")==end_line&&xjson_get_int(finish,"character")==end);
    return xjson_get(node,"parent");
}
static void facts(XrJsonValue *result,unsigned mode) {
    CHECK(result&&result->type==XR_JSON_ARRAY&&result->as.array.count==(mode?1:3));
    XrJsonValue *node=xjson_array_get(result,0);
    if(mode==0) {
        node=range_fact(node,0,0,0,7);CHECK(!range_fact(node,0,0,2,0));
        node=xjson_array_get(result,1);node=range_fact(node,1,2,1,7);node=range_fact(node,1,0,1,7);CHECK(!range_fact(node,0,0,2,0));
        node=xjson_array_get(result,2);node=range_fact(node,2,0,2,0);CHECK(!range_fact(node,0,0,2,0));
    } else if(mode==1)CHECK(!range_fact(node,0,0,0,0));
    else if(mode==2)CHECK(!range_fact(node,0,0,0,4));
    else CHECK(!range_fact(node,0,0,0,6));
}
static XrXirStatus once(unsigned mode,XrCompileResourceLimits caps,XrCompileResourceStats *stats,size_t fault,bool *injected) {
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    XrJsonValue *input=xjson_parse(positions[mode],strlen(positions[mode]));CHECK(input);
    XrCompileResources *resources=NULL;XrJsonValue *result=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(&caps,&resources);
    XrXirStatus status=made==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char text[64];size_t length=strlen(texts[mode]);CHECK(length<sizeof(text));memcpy(text,texts[mode],length+1);
    lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;
    if(status==XR_XIR_OK)status=xlsp_source_selection_json(resources,text,length,input,&result);
    if(stats&&status!=XR_XIR_OK)fprintf(stderr,"Selection diagnostic mode=%u status=%u count=%d capacity=%d\n",mode,(unsigned)status,input->as.array.count,input->as.array.capacity);
    if(injected)*injected=lsp_json_injected;lsp_json_fail=SIZE_MAX;lsp_json_injected=false;memset(text,'?',sizeof(text));
    if(status==XR_XIR_OK) {
        facts(result,mode);XrJsonValue *occupied=result;size_t before=runtime_attempts,json_before=lsp_json_calls;
        CHECK(xlsp_source_selection_json(resources,text,length,input,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==result&&runtime_attempts==before&&lsp_json_calls==json_before);
    } else CHECK(!result);
    if(stats)*stats=sdk_stats(resources);
    xr_compile_resources_release(resources);xjson_free(input);CHECK(!runtime_live&&!runtime_bytes);
    if(status==XR_XIR_OK)facts(result,mode);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);return status;
}
static void invalid_inputs(void) {
    static const char *const invalid[]={"[{\"line\":0,\"character\":2}]","[{\"line\":0,\"character\":8}]",
        "[{\"line\":3,\"character\":0}]","[{\"line\":-1,\"character\":0}]","[{\"line\":0,\"character\":1.5}]",
        "[null]","[{\"line\":0}]","{}","[{\"line\":0,\"character\":4},{\"line\":0,\"character\":2}]"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        XrCompileResources *resources=sdk_ledger(&limits);XrJsonValue *input=xjson_parse(invalid[i],strlen(invalid[i])),*result=NULL;CHECK(input);
        CHECK(xlsp_source_selection_json(resources,texts[0],strlen(texts[0]),input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
        xjson_free(input);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    }
    XrCompileResources *resources=sdk_ledger(&limits);XrJsonValue *input=xjson_parse("[]",2),*result=NULL;CHECK(input);
    CHECK(xlsp_source_selection_json(resources,"\x80",1,input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    CHECK(xlsp_source_selection_json(resources,"\0",1,input,&result)==XR_XIR_BAD_STRUCTURE&&!result);
    CHECK(xlsp_source_selection_json(resources,"",0,input,&result)==XR_XIR_OK&&result&&result->as.array.count==0);
    xjson_free(input);xjson_free(result);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool full=argc==1;
    for(unsigned mode=0;mode<4;++mode) {
        XrCompileResourceStats stats={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
        CHECK(once(mode,limits,&stats,SIZE_MAX,NULL)==XR_XIR_OK);size_t sites=runtime_attempts,json_sites=lsp_json_calls;CHECK(sites&&json_sites);
        if(full) {
            for(size_t i=0;i<sites;++i) {runtime_attempts=0;runtime_fail_at=i;CHECK(once(mode,limits,NULL,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>i);}
            runtime_fail_at=SIZE_MAX;
            for(size_t i=0;i<json_sites;++i) {bool injected=false;CHECK(once(mode,limits,NULL,i,&injected)==XR_XIR_OUT_OF_MEMORY&&injected);}
            for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
                XrCompileResourceLimits exact={stats.allocated_bytes,stats.peak_bytes,stats.work};
                uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
                CHECK(once(mode,exact,NULL,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
            }
        }
        printf("Source selection mode=%u SourceFI=%zu JSONFI=%zu full=%u exactUTF16/EOF, producer death, physical=0/0\n",mode,sites,json_sites,full?1u:0u);
    }
    invalid_inputs();return 0;
}
