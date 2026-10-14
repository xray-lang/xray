/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_index_store.c - Real syntax owners, atomic receipt and component removal
 */
#include "app/lsp/xlsp_source_index_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_sdk_resource_test.h"
static const XrCompileResourceLimits store_limits={UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
static XrXirStatus store_build(XrCompileResources *resources,unsigned mode,XlspSourceIndex **out) {
    const char *uris[]={"file:///E:/scope/a.xr","file:///E:/scope/b.xr","file:///E:/scope-neighbor/c.xr","file:///E:/scope/a.xr"};
    const char *texts[]={"fn shared()->i64 { return 1 }\n","export fn shared()->i64 { return 2 }\n","fn other()->i64 { return 3 }\n","fn updated()->i64 { return 4 }\n"};
    char uri[64],text[80];strcpy(uri,uris[mode]);strcpy(text,texts[mode]);
    XlspSourceDocument input={uri,text,strlen(uri),strlen(text),(int64_t)mode+11};
    XrXirStatus status=xlsp_source_index_build(resources,&input,out);
    memset(uri,'?',sizeof(uri));memset(text,'?',sizeof(text));memset(&input,0,sizeof(input));return status;
}
static XrXirStatus store_visit(const XlspIndexedSymbol *view,void *context) {
    size_t *count=context;CHECK(view&&view->uri&&view->symbol&&view->symbol->name);
    CHECK(view->symbol->selection.start.line==0&&view->symbol->selection.end.line==0);
    CHECK(view->symbol->selection.start.character==(view->symbol->exported?10u:3u));
    CHECK(view->version>=11&&view->version<=14);++*count;return XR_XIR_OK;
}
static XrXirStatus store_reject_visit(const XlspIndexedSymbol *view,void *context) {
    size_t *count=context;CHECK(view&&view->symbol);++*count;return XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus store_trace(XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XlspSourceIndexStore *store=NULL;XlspSourceIndex *owners[4]={0};
    XrXirStatus status=XR_XIR_OK;bool external=true;XlspIndexedSymbol original={0};
    XrCompileResourceStatus made=xr_compile_resources_new(&limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    for(unsigned i=0;i<4&&status==XR_XIR_OK;++i)status=store_build(resources,i,&owners[i]);
    if(status!=XR_XIR_OK)goto done;
    status=xlsp_source_index_store_new(resources,&store);if(status!=XR_XIR_OK)goto done;
    for(unsigned i=0;i<3;++i) {
        XlspSourceIndex *input=owners[i];size_t before=xlsp_source_index_store_count(store);
        status=xlsp_source_index_store_replace(store,&owners[i]);
        if(status!=XR_XIR_OK){CHECK(owners[i]==input&&xlsp_source_index_store_count(store)==before);goto done;}
        CHECK(!owners[i]&&xlsp_source_index_store_count(store)==i+1);
    }
    xr_compile_resources_release(resources);external=false;
    status=xlsp_source_index_store_find(store,"shared",&original);if(status!=XR_XIR_OK)goto done;
    CHECK(original.symbol&&original.symbol->exported&&!strcmp(original.uri,"file:///E:/scope/b.xr")&&original.version==12);
    CHECK(original.symbol->selection.start.character==10&&original.symbol->selection.end.character==16);
    {
        size_t count=0;status=xlsp_source_index_store_visit(store,store_visit,&count);if(status!=XR_XIR_OK)goto done;CHECK(count==3);
        count=0;XrXirStatus rejected=xlsp_source_index_store_visit(store,store_reject_visit,&count);
        if(rejected==XR_XIR_BUDGET){status=rejected;goto done;}CHECK(rejected==XR_XIR_BAD_STRUCTURE&&count==1);
    }
    {
        XlspSourceIndex *input=owners[3];status=xlsp_source_index_store_replace(store,&owners[3]);
        CHECK(!strcmp(original.symbol->name,"shared")&&original.version==12);
        if(status!=XR_XIR_OK){CHECK(owners[3]==input&&xlsp_source_index_store_count(store)==3);goto done;}
        CHECK(!owners[3]&&xlsp_source_index_store_count(store)==3);
        XlspIndexedSymbol updated={0};status=xlsp_source_index_store_find(store,"updated",&updated);if(status!=XR_XIR_OK)goto done;
        CHECK(updated.version==14&&!updated.symbol->exported&&updated.symbol->selection.start.character==3&&updated.symbol->selection.end.character==10);
    }
    {
        XlspIndexedSymbol missing=original;status=xlsp_source_index_store_find(store,"absent",&missing);
        CHECK(missing.symbol==original.symbol&&missing.uri==original.uri&&missing.version==original.version);
        if(status==XR_XIR_BUDGET)goto done;CHECK(status==XR_XIR_UNRESOLVED);status=XR_XIR_OK;
    }
    status=xlsp_source_index_store_remove_prefix(store,"file:///E:/scope");
    if(status!=XR_XIR_OK){CHECK(xlsp_source_index_store_count(store)==3&&!strcmp(original.symbol->name,"shared"));goto done;}
    CHECK(xlsp_source_index_store_count(store)==1);
    {
        XlspIndexedSymbol neighbor={0};status=xlsp_source_index_store_find(store,"other",&neighbor);if(status!=XR_XIR_OK)goto done;
        CHECK(!strcmp(neighbor.uri,"file:///E:/scope-neighbor/c.xr")&&neighbor.version==13);
        status=xlsp_source_index_store_remove(store,"file:///E:/missing.xr");if(status!=XR_XIR_OK)goto done;
        CHECK(xlsp_source_index_store_count(store)==1);
        status=xlsp_source_index_store_remove(store,"file:///E:/scope-neighbor/c.xr");
        if(status!=XR_XIR_OK){CHECK(xlsp_source_index_store_count(store)==1&&!strcmp(neighbor.symbol->name,"other"));goto done;}
        CHECK(!xlsp_source_index_store_count(store));
    }
done:
    if(stats)*stats=sdk_stats(resources);
    if(external)xr_compile_resources_release(resources);
    for(unsigned i=0;i<4;++i)xlsp_source_index_free(owners[i]);xlsp_source_index_store_free(store);
    CHECK(!runtime_live&&!runtime_bytes);return status;
}
static void store_bad_arguments(void) {
    XrCompileResources *resources=sdk_ledger(&store_limits);XlspSourceIndexStore *store=NULL;
    CHECK(xlsp_source_index_store_new(resources,&store)==XR_XIR_OK);
    XlspSourceIndexStore *occupied=store;size_t before=runtime_attempts;
    CHECK(xlsp_source_index_store_new(resources,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==store&&runtime_attempts==before);
    XlspSourceIndex *empty=NULL;XlspIndexedSymbol view={0};
    CHECK(xlsp_source_index_store_replace(store,&empty)==XR_XIR_BAD_STRUCTURE&&!empty);
    CHECK(xlsp_source_index_store_remove_prefix(store,"")==XR_XIR_BAD_STRUCTURE);
    CHECK(xlsp_source_index_store_find(store,NULL,&view)==XR_XIR_BAD_STRUCTURE&&!view.symbol);
    CHECK(xlsp_source_index_store_visit(store,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
    xlsp_source_index_store_free(store);xr_compile_resources_release(resources);CHECK(!runtime_live&&!runtime_bytes);
}
int main(int argc,char **argv) {
    (void)sdk_fixture_malloc;(void)sdk_fixture_free;(void)sdk_unlimited;
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool faults=argc==1;
    runtime_attempts=0;runtime_fail_at=SIZE_MAX;XrCompileResourceStats needed={0};
    CHECK(store_trace(store_limits,&needed)==XR_XIR_OK);size_t sites=runtime_attempts;
    if(faults)for(size_t site=0;site<sites;++site) {
        runtime_attempts=0;runtime_fail_at=site;CHECK(store_trace(store_limits,NULL)==XR_XIR_OUT_OF_MEMORY&&runtime_attempts>site);
    }
    runtime_fail_at=SIZE_MAX;
    if(faults)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits exact={needed.allocated_bytes,needed.peak_bytes,needed.work};
        uint64_t *bound=axis==0?&exact.allocated_bytes:axis==1?&exact.live_bytes:&exact.work;CHECK(*bound&&*bound<INT64_MAX);
        *bound=(uint64_t)((int64_t)*bound+delta);CHECK(store_trace(exact,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    store_bad_arguments();
    printf("LSP real syntax index store Source/receipt FI=%zu; owner death, atomic replacement, exact ranges, component removal, physical=0/0; full=%d\n",sites,faults?1:0);
    return 0;
}
