/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_watch.h"
#include "app/lsp/xlsp_source_workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char *const cases[]={
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a%20b.xr\",\"type\":1},{\"uri\":\"file:///C:/watch/a%20b.xr\",\"type\":2},{\"uri\":\"file:///C:/watch/a%20b.xr\",\"type\":3}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/%CE%BB%F0%9F%98%80.xr\",\"type\":2}]}",
    "{\"changes\":[]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":1}]}",
    "{}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":1},{\"uri\":\"file:///C:/watch/b.xr\",\"type\":0}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a\\u0000b.xr\",\"type\":1}]}",
    "{\"changes\":[{\"uri\":\"https://example.invalid/a.xr\",\"type\":1}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":true}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":1.5}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":4294967297}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"uri\":\"file:///C:/watch/b.xr\",\"type\":1}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":1,\"type\":2}]}",
    "{\"changes\":[],\"changes\":[]}",
    "{\"changes\\u0000bad\":[]}",
    "{\"changes\":[{\"type\":1}]}",
    "{\"changes\":[null]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":1}]}",
    "{\"changes\":[{\"uri\":\"\",\"type\":1}]}",
    "{\"future\":null,\"changes\":[{\"uri\":\"file:///C:/watch/a.xr\",\"type\":2.0,\"future\":null}]}",
    "{\"changes\":[{\"uri\":\"file:///C:/watch/%FF.xr\",\"type\":1}]}"
};
static bool valid(unsigned mode){return mode<4||mode==19;}
static XrJsonValue *input(unsigned mode){
    XrJsonValue *root=xjson_parse(cases[mode],strlen(cases[mode]));CHECK(root);
    if(mode==3){char uri[5040];const char *prefix="file:///C:/watch/";size_t length=strlen(prefix);memcpy(uri,prefix,length);memset(uri+length,'x',5000);memcpy(uri+length+5000,".xr",4);
        XrJsonValue *event=xjson_array_get(xjson_get_array(root,"changes"),0);xjson_object_set(event,"uri",xjson_new_string(uri));CHECK(xjson_get(event,"uri")->string_len==5020);}
    if(mode==17){XrJsonValue *number=xjson_get(xjson_array_get(xjson_get_array(root,"changes"),0),"type");number->is_integer=false;number->as.number=NAN;}
    return root;
}
static XrXirStatus facts(const XlspSourceWatch *owner,unsigned mode){
    size_t count=SIZE_MAX;const XlspSourceWatchEvent *items=xlsp_source_watch_items(owner,&count);CHECK(count==(mode==0?3:mode==2?0:1));
    if(!count)CHECK(!items);
    for(size_t i=0;i<count;++i){CHECK(items[i].uri&&items[i].path&&items[i].index_uri);CHECK(items[i].kind==(mode==0?(uint32_t)i+1:mode==3?1u:2u));
        if(mode==0){CHECK(!strcmp(items[i].uri,"file:///C:/watch/a%20b.xr"));CHECK(strstr(items[i].path,"a b.xr"));CHECK(strstr(items[i].index_uri,"a%20b.xr"));}
        if(mode==1){CHECK(strstr(items[i].path,"λ😀.xr"));CHECK(strstr(items[i].index_uri,"%CE%BB%F0%9F%98%80.xr"));}
        if(mode==3)CHECK(strlen(items[i].uri)==5020&&strlen(items[i].path)>=5003&&strstr(items[i].index_uri,".xr"));
    }
    XrLspServer server={0};server.source_revision=10;XrXirStatus status=xlsp_source_watch_commit(owner,&server);
    CHECK(!server.source_workspace);CHECK(server.source_revision==(status==XR_XIR_OK?10+count:10));return status;
}
static XrXirStatus once(unsigned mode,const XrCompileResourceLimits *caps,size_t fail,XrCompileResourceStats *stats){
    XrJsonValue *json=input(mode);size_t json_calls=lsp_json_calls;runtime_attempts=0;runtime_fail_at=fail;
    XrCompileResources *r=NULL;XrCompileResourceStatus opened=xr_compile_resources_new(caps,&r);
    XrXirStatus status=opened==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:opened==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XlspSourceWatch *owner=NULL;if(status==XR_XIR_OK)status=xlsp_source_watch_read(r,json,&owner);xjson_free(json);
    if(status==XR_XIR_OK)status=facts(owner,mode);else CHECK(!owner);
    CHECK(lsp_json_calls==json_calls&&!lsp_json_live&&!lsp_json_bytes);
    if(stats&&r)*stats=sdk_stats(r);xlsp_source_watch_free(owner);xr_compile_resources_release(r);runtime_fail_at=SIZE_MAX;CHECK(!runtime_live&&!runtime_bytes);return status;
}
static void lifetime(const char *entry){
    XrJsonValue *json=input(0);XrCompileResources *r=sdk_ledger(&limits);XlspSourceWatch *owner=NULL;CHECK(xlsp_source_watch_read(r,json,&owner)==XR_XIR_OK);
    XlspSourceWatch *occupied=owner;size_t calls=runtime_attempts;CHECK(xlsp_source_watch_read(r,json,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==owner&&runtime_attempts==calls);
    const char text[]="fn kept()->i64{return 1}\n";XlspSourceDocument document={entry,text,strlen(entry),sizeof(text)-1,1};
    XrLspServer server={0};XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    CHECK(xlsp_source_workspace_build(&context,&document,1,&server.source_workspace,NULL,NULL)==XR_XIR_OK);
    XlspSourceWorkspace *kept=server.source_workspace;size_t live=runtime_live,bytes=runtime_bytes;
    xjson_free(json);xr_compile_resources_release(r);CHECK(facts(owner,0)==XR_XIR_OK);
    server.source_revision=UINT64_MAX;CHECK(xlsp_source_watch_commit(owner,&server)==XR_XIR_BUDGET);
    CHECK(server.source_revision==UINT64_MAX&&server.source_workspace==kept&&runtime_live==live&&runtime_bytes==bytes);
    server.source_revision=UINT64_MAX-3;CHECK(xlsp_source_watch_commit(owner,&server)==XR_XIR_OK&&server.source_revision==UINT64_MAX&&!server.source_workspace);
    xlsp_source_watch_free(owner);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    json=input(2);r=sdk_ledger(&limits);owner=NULL;CHECK(xlsp_source_watch_read(r,json,&owner)==XR_XIR_OK);
    context=(XrXirCompileContext){r,xr_xir_compile_default_limits()};server=(XrLspServer){0};
    CHECK(xlsp_source_workspace_build(&context,&document,1,&server.source_workspace,NULL,NULL)==XR_XIR_OK);kept=server.source_workspace;
    xjson_free(json);xr_compile_resources_release(r);server.source_revision=UINT64_MAX;
    CHECK(xlsp_source_watch_commit(owner,&server)==XR_XIR_OK&&server.source_revision==UINT64_MAX&&server.source_workspace==kept);
    xlsp_source_workspace_free(server.source_workspace);server.source_workspace=NULL;
    xlsp_source_watch_free(owner);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
int main(int argc,char **argv){
    CHECK(argc==2);
    (void)sdk_unlimited;bool full=getenv("XR_LSP_WATCH_NORMAL_ONLY")==NULL;
    for(unsigned mode=0;mode<sizeof(cases)/sizeof(*cases);++mode){XrCompileResourceStats stats={0};CHECK(once(mode,&limits,SIZE_MAX,&stats)==(valid(mode)?XR_XIR_OK:XR_XIR_BAD_STRUCTURE));size_t sites=runtime_attempts;
        if(full){for(size_t i=0;i<sites;++i)CHECK(once(mode,&limits,i,NULL)==XR_XIR_OUT_OF_MEMORY);
            if(valid(mode))for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits caps=limits;uint64_t value=axis==0?stats.allocated_bytes:axis==1?stats.peak_bytes:stats.work;value=delta<0?value-1:value+(unsigned)delta;
                if(!axis)caps.allocated_bytes=value;else if(axis==1)caps.live_bytes=value;else caps.work=value;CHECK(once(mode,&caps,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
        printf("Watch mode=%u SourceFI=%zu JSONallocations=0 full=%u ordered owned URI/path/revision physical0/0\n",mode,sites,full?1u:0u);}
    lifetime(argv[1]);return 0;
}
