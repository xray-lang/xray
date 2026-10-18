/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_sdk_resource_test.h"
static const char index_text[]=
    "/* \xf0\x9f\x98\x80 */ export fn outer()->i64 {\n"
    " var local=1\n return local\n}\n"
    "export struct Box {\n const value:i64\n read()->i64 { return this.value }\n}\n"
    "export enum Tone {\n Red,\n Blue\n}\n"
    "interface Ready { read()->i64 }\n";
static const char index_recovered[]="export fn kept()->i64 { return 1 }\nvar *** = ;\n";
static const char index_uri[]="file:///E:/source-index-owner/sample.xr";
static void index_facts(const XlspSourceIndex *index,unsigned mode) {
    CHECK(index&&!strcmp(xlsp_source_index_uri(index),index_uri)&&xlsp_source_index_version(index)==19);
    CHECK(xlsp_source_index_parse_status(index)==(mode?XR_PARSE_RECOVERED:XR_PARSE_OK));
    size_t count=0;const XlspSourceIndexSymbol *items=xlsp_source_index_symbols(index,&count);CHECK(items);
    if(mode){CHECK(count==1&&!strcmp(items[0].name,"kept")&&items[0].exported&&items[0].selection.start.line==0&&items[0].selection.start.character==10);return;}
    const char *names[]={"outer","local","Box","value","read","Tone","Red","Blue","Ready"};
    const uint32_t kinds[]={12,13,5,8,6,10,22,22,11},rows[]={0,1,4,5,6,8,9,10,12};
    const uint32_t starts[]={19,5,14,7,1,12,1,1,10},ends[]={24,10,17,12,5,16,4,5,15};
    const bool exported[]={true,false,true,false,false,true,false,false,false};CHECK(count==9);
    for(size_t i=0;i<count;++i) {
        CHECK(!strcmp(items[i].name,names[i])&&(uint32_t)items[i].kind==kinds[i]&&items[i].exported==exported[i]);
        CHECK(items[i].selection.start.line==rows[i]&&items[i].selection.end.line==rows[i]&&
            items[i].selection.start.character==starts[i]&&items[i].selection.end.character==ends[i]);
    }
}
static XrXirStatus index_once(unsigned mode,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XlspSourceIndex *index=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char text[sizeof(index_text)],uri[sizeof(index_uri)];strcpy(text,mode?index_recovered:index_text);memcpy(uri,index_uri,sizeof(uri));
    XlspSourceDocument input={uri,text,sizeof(uri)-1,strlen(text),19};
    XrXirStatus status=xlsp_source_index_build(resources,&input,&index);
    memset(text,'?',sizeof(text));memset(uri,'?',sizeof(uri));memset(&input,0,sizeof(input));
    if(status==XR_XIR_OK) {
        index_facts(index,mode);XlspSourceIndex *occupied=index;size_t before=runtime_attempts;
        CHECK(xlsp_source_index_build(resources,&input,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==index&&before==runtime_attempts);
    } else CHECK(!index);
    if(stats)*stats=sdk_stats(resources);xr_compile_resources_release(resources);
    if(index)index_facts(index,mode);xlsp_source_index_free(index);CHECK(!runtime_live&&!runtime_bytes);return status;
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_ledger;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool faults=argc==1;
    XrCompileResourceLimits limits={UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
    for(unsigned mode=0;mode<2;++mode) {
        size_t sites=0;XrCompileResourceStats needed={0};
        for(size_t probe=0;probe<=sites;++probe) {
            runtime_attempts=0;runtime_fail_at=probe?probe-1:SIZE_MAX;
            XrXirStatus status=index_once(mode,limits,probe?NULL:&needed);
            if(!probe){CHECK(status==XR_XIR_OK);if(faults)sites=runtime_attempts;}
            else CHECK(status==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>=probe);
        }
        runtime_fail_at=SIZE_MAX;
        if(faults)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
            XrCompileResourceLimits exact={needed.allocated_bytes,needed.peak_bytes,needed.work};
            uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);
            *bound=(uint64_t)((int64_t)*bound+delta);CHECK(index_once(mode,exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
        }
        printf("LSP real syntax index mode=%u Source FI=%zu, exact metadata/export/UTF16, complete owner death, physical=0/0\n",mode,sites);
    }
    return 0;
}
