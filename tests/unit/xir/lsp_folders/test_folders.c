/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_folders.h"
#include "app/lsp/xlsp_source_workspace.h"
#include "app/lsp/xlsp_source_index_store.h"
#include "app/lsp/xlsp_index_pool.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_sdk_resource_test.h"
#include "lsp_open_json_owner.h"
static char *app_native_strdup(const char *s){return xr_strdup(s);}
static SdkAllocation app_blocks[256];
static size_t app_attempts,app_fail_at=SIZE_MAX,app_live,app_bytes;
static char *app_strdup(const char *s){
    if(app_attempts++==app_fail_at)return NULL;
    char *p=app_native_strdup(s);if(!p)return NULL;
    for(size_t i=0;i<256;++i)if(!app_blocks[i].pointer){app_blocks[i]=(SdkAllocation){p,strlen(s)+1};++app_live;app_bytes+=strlen(s)+1;return p;}
    CHECK(false);return NULL;
}
static void *app_malloc(size_t bytes){
    if(app_attempts++==app_fail_at)return NULL;
    void *p=sdk_fixture_malloc(bytes);if(!p)return NULL;
    for(size_t i=0;i<256;++i)if(!app_blocks[i].pointer){app_blocks[i]=(SdkAllocation){p,bytes};++app_live;app_bytes+=bytes;return p;}
    CHECK(false);return NULL;
}
static void app_free(void *p){
    if(!p)return;
    for(size_t i=0;i<256;++i)if(app_blocks[i].pointer==p){--app_live;app_bytes-=app_blocks[i].bytes;app_blocks[i]=(SdkAllocation){0};sdk_fixture_free(p);return;}
    CHECK(false);
}
#undef xr_strdup
#undef xr_free
#undef xr_malloc
#define xr_strdup app_strdup
#define xr_free app_free
#define xr_malloc app_malloc
#include "app/lsp/xlsp_source_folders.c"
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
#define FOLDER_A "{\"uri\":\"file:///C:/roots/a\",\"name\":\"A\"}"
#define FOLDER_B "{\"uri\":\"file:///C:/roots/b\",\"name\":\"B\"}"
#define FOLDER_C "{\"uri\":\"file:///C:/roots/c%20%CE%BB\",\"name\":\"C λ😀\"}"
static const char *const cases[]={
    "{\"event\":{\"removed\":[" FOLDER_A "],\"added\":[" FOLDER_C "]}}",
    "{\"event\":{\"removed\":[" FOLDER_A "," FOLDER_B "],\"added\":[]}}",
    "{\"event\":{\"removed\":[],\"added\":[]}}",
    "{\"event\":{\"removed\":[" FOLDER_C "],\"added\":[]}}",
    "{\"event\":{\"removed\":[" FOLDER_A "],\"added\":[{\"uri\":\"file:///C:/roots/a\",\"name\":\"renamed\"}]}}",
    "{\"event\":{\"removed\":[" FOLDER_A "],\"added\":[" FOLDER_C ",{\"uri\":false,\"name\":\"bad\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":[" FOLDER_A "]}}",
    "{\"event\":{\"removed\":[],\"added\":[" FOLDER_C "," FOLDER_C "]}}",
    "{\"event\":{\"removed\":[" FOLDER_A "," FOLDER_A "],\"added\":[]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"file:///C:/x\",\"name\":\"a\\u0000b\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"file:///C:/a\\u0000b\",\"name\":\"x\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"https://example.invalid/a\",\"name\":\"x\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"file:///C:/x\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":null}}",
    "{\"event\":{\"removed\":[],\"added\":[],\"added\":[]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"file:///C:/x\",\"name\":\"x\",\"uri\":\"file:///C:/y\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"file:///C:/x\",\"name\":\"x\",\"name\":\"y\"}]}}",
    "{\"event\\u0000x\":{\"removed\":[],\"added\":[]}}",
    "{\"event\":{\"removed\":[],\"added\":[{\"uri\":\"file:///C:/%FF\",\"name\":\"x\"}]}}",
    "{\"event\":{\"removed\":[],\"added\":[" FOLDER_C "]}}",
    "{\"event\":{\"removed\":[],\"added\":[" FOLDER_C "]}}",
    "{\"event\":{\"removed\":[],\"added\":[" FOLDER_C "]}}"
};
static XrXirStatus expected(unsigned mode){return mode<5||mode==20?XR_XIR_OK:mode==19?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;}
static void initial(XrLspServer *server,unsigned mode){
    server->source_revision=10;server->workspace_folder_count=mode==19?MAX_WORKSPACE_FOLDERS:2;
    for(int i=0;i<server->workspace_folder_count;++i){char uri[80],path[80];char letter=(char)('a'+i);
        snprintf(uri,sizeof(uri),"file:///C:/roots/%c",letter);snprintf(path,sizeof(path),"C:/roots/%c",letter);
        server->workspace_folders[i]=(XlspWorkspaceFolder){app_strdup(uri),app_strdup(i?"B":"A"),app_strdup(path),true,false,true};
        CHECK(server->workspace_folders[i].uri&&server->workspace_folders[i].name&&server->workspace_folders[i].path);}
    server->files_total=42;server->files_indexed=19;server->indexing_in_progress=true;
}
static void cleanup(XrLspServer *server){
    for(int i=0;i<server->workspace_folder_count;++i)folders_release(&server->workspace_folders[i],true);
    xlsp_index_pool_free(server->index_pool);xlsp_source_index_store_free(server->source_index);xlsp_source_workspace_free(server->source_workspace);
}
static XrJsonValue *input(unsigned mode){
    XrJsonValue *root=xjson_parse(cases[mode],strlen(cases[mode]));CHECK(root);
    if(mode==20){char uri[5100];const char *prefix="file:///C:/roots/";size_t length=strlen(prefix);memcpy(uri,prefix,length);memset(uri+length,'x',5000);uri[length+5000]=0;
        xjson_object_set(xjson_array_get(xjson_get_array(xjson_get_object(root,"event"),"added"),0),"uri",xjson_new_string(uri));}
    if(mode==21){XrJsonValue *name=xjson_get(xjson_array_get(xjson_get_array(xjson_get_object(root,"event"),"added"),0),"name");((unsigned char *)name->as.string)[0]=0xff;}
    return root;
}
static XrXirStatus once(unsigned mode,const XrCompileResourceLimits *caps,size_t fail,size_t app_fail,XrCompileResourceStats *stats){
    XrLspServer server={0};initial(&server,mode);app_attempts=0;app_fail_at=app_fail;
    XrJsonValue *json=input(mode);size_t json_calls=lsp_json_calls;runtime_attempts=0;runtime_fail_at=fail;
    XrCompileResources *r=NULL;XrCompileResourceStatus opened=xr_compile_resources_new(caps,&r);
    XrXirStatus status=opened==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:opened==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    server.index_resources=r;if(status==XR_XIR_OK)status=xlsp_source_index_store_new(r,&server.source_index);
    XrLspServer before=server;bool changed=false;XlspSourceFolders *owner=NULL;
    if(status==XR_XIR_OK)status=xlsp_source_folders_read(r,&server,json,&owner);xjson_free(json);
    if(status==XR_XIR_OK)status=xlsp_source_folders_commit(owner,&server,&changed);
    if(status!=XR_XIR_OK||!changed)CHECK(!memcmp(&before,&server,sizeof(server))&&!changed);
    else{CHECK(server.source_revision==11&&!server.index_pool&&!server.source_workspace&&server.source_index!=before.source_index);
        CHECK(!server.indexing_in_progress&&server.index_cancelled&&!server.files_total&&!server.files_indexed);
        CHECK(server.workspace_folder_count==(mode==1?0:mode==20?3:2));
        for(int i=0;i<server.workspace_folder_count;++i)CHECK(server.workspace_folders[i].index_requested&&!server.workspace_folders[i].index_completed);
        if(mode==0)CHECK(!strcmp(server.workspace_folders[0].uri,"file:///C:/roots/b")&&strstr(server.workspace_folders[1].path,"c λ")&&!server.workspace_folders[1].config_loaded);
        if(mode==4)CHECK(!strcmp(server.workspace_folders[1].name,"renamed")&&!server.workspace_folders[1].config_loaded);
        if(mode==20)CHECK(strlen(server.workspace_folders[2].uri)>5000&&strlen(server.workspace_folders[2].path)>5000);
    }
    CHECK(lsp_json_calls==json_calls&&!lsp_json_live&&!lsp_json_bytes);
    if(stats&&r)*stats=sdk_stats(r);xlsp_source_folders_free(owner);cleanup(&server);xr_compile_resources_release(r);
    runtime_fail_at=app_fail_at=SIZE_MAX;CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes);return status;
}
static void lifetime(const char *entry){
    XrLspServer server={0};initial(&server,0);XrCompileResources *r=sdk_ledger(&limits);server.index_resources=r;
    CHECK(xlsp_source_index_store_new(r,&server.source_index)==XR_XIR_OK);
    const char text[]="fn kept()->i64{return 1}\n";XlspSourceDocument doc={entry,text,strlen(entry),sizeof(text)-1,1};
    XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    CHECK(xlsp_source_workspace_build(&context,&doc,1,&server.source_workspace,NULL,NULL)==XR_XIR_OK);
    XrJsonValue *json=input(0);XlspSourceFolders *owner=NULL;CHECK(xlsp_source_folders_read(r,&server,json,&owner)==XR_XIR_OK);
    xjson_free(json);xr_compile_resources_release(r);
    XlspIndexPoolOptions options={r,50u*1024u*1024u,2};CHECK(xlsp_index_pool_new(&options,&server.index_pool)==XR_XIR_OK);
    char *path=NULL;CHECK(xlsp_source_uri_path(r,entry,strlen(entry),&path)==XR_XIR_OK);
    CHECK(xlsp_index_pool_submit(server.index_pool,path,entry,10)==XR_XIR_OK);xr_compile_resources_free(path);
    for(unsigned wait=0;!xlsp_index_pool_is_idle(server.index_pool);++wait){CHECK(wait<1000);xr_time_sleep_ms(1);}
    uint64_t submitted=0,completed=0;xlsp_index_pool_get_progress(server.index_pool,&submitted,&completed);CHECK(submitted==1&&completed==1);
    XrLspIndexResult *result=xlsp_index_pool_poll(server.index_pool);CHECK(result&&result->status==XR_XIR_OK&&result->index&&!result->next);
    CHECK(xlsp_source_index_store_replace(server.source_index,&result->index)==XR_XIR_OK&&xlsp_source_index_store_count(server.source_index)>0);
    CHECK(xlsp_index_pool_submit(server.index_pool,result->path,result->uri,10)==XR_XIR_OK);xlsp_index_result_free_list(result);
    for(unsigned wait=0;!xlsp_index_pool_is_idle(server.index_pool);++wait){CHECK(wait<1000);xr_time_sleep_ms(1);}
    xlsp_index_pool_get_progress(server.index_pool,&submitted,&completed);CHECK(submitted==2&&completed==2);
    XrLspServer before=server;bool changed=false;server.source_revision=11;size_t calls=app_attempts;
    CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_UNRESOLVED&&!changed&&app_attempts==calls);server=before;
    server.workspace_folders[0].config_loaded=false;
    CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_UNRESOLVED&&!changed&&app_attempts==calls);server=before;
    CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_OK&&changed&&!server.index_pool&&!server.source_workspace);
    CHECK(!xlsp_source_index_store_count(server.source_index));
    calls=app_attempts;CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_UNRESOLVED&&app_attempts==calls);
    xlsp_source_folders_free(owner);cleanup(&server);CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes);
    server=(XrLspServer){0};initial(&server,0);r=sdk_ledger(&limits);server.index_resources=r;CHECK(xlsp_source_index_store_new(r,&server.source_index)==XR_XIR_OK);
    server.source_revision=UINT64_MAX;owner=NULL;CHECK(xlsp_source_folders_single(r,&server,"file:///C:/roots/z","z",true,&owner)==XR_XIR_OK);
    before=server;changed=false;CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_BUDGET&&!memcmp(&server,&before,sizeof(server))&&!changed);
    xlsp_source_folders_free(owner);owner=NULL;CHECK(xlsp_source_folders_single(r,&server,"file:///C:/roots/z","",false,&owner)==XR_XIR_OK);
    CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_OK&&!changed&&!memcmp(&server,&before,sizeof(server)));
    xlsp_source_folders_free(owner);cleanup(&server);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes);
}
static void receiver_budget(void){
    XrLspServer server={0};initial(&server,0);XrCompileResources *r=sdk_ledger(&limits);server.index_resources=r;
    CHECK(xlsp_source_index_store_new(r,&server.source_index)==XR_XIR_OK);XlspSourceFolders *owner=NULL;
    CHECK(xlsp_source_folders_single(r,&server,"file:///C:/roots/z","Z",true,&owner)==XR_XIR_OK);
    XrCompileResourceLimits closed={sizeof(XrCompileResources),sizeof(XrCompileResources),UINT64_MAX};XrCompileResources *receiver=sdk_ledger(&closed);server.index_resources=receiver;
    XrLspServer before=server;bool changed=false;size_t live=app_live,bytes=app_bytes;
    CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_BUDGET&&!changed&&!memcmp(&server,&before,sizeof(server))&&app_live==live&&app_bytes==bytes);
    XrCompileResourceStats stats=sdk_stats(receiver);CHECK(stats.allocated_bytes==closed.allocated_bytes&&stats.live_bytes==closed.live_bytes);
    server.index_resources=r;CHECK(xlsp_source_folders_commit(owner,&server,&changed)==XR_XIR_OK&&changed&&server.workspace_folder_count==3);
    xlsp_source_folders_free(owner);cleanup(&server);xr_compile_resources_release(receiver);xr_compile_resources_release(r);
    CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes);
}
int main(int argc,char **argv){
    CHECK(argc==2);(void)sdk_unlimited;bool full=getenv("XR_LSP_FOLDERS_NORMAL_ONLY")==NULL;
    for(unsigned mode=0;mode<sizeof(cases)/sizeof(*cases);++mode){XrCompileResourceStats stats={0};CHECK(once(mode,&limits,SIZE_MAX,SIZE_MAX,&stats)==expected(mode));size_t sites=runtime_attempts,app_sites=app_attempts;
        if(full){for(size_t i=0;i<sites;++i)CHECK(once(mode,&limits,i,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY);
            for(size_t i=0;i<app_sites;++i)CHECK(once(mode,&limits,SIZE_MAX,i,NULL)==XR_XIR_OUT_OF_MEMORY);
            if(expected(mode)==XR_XIR_OK)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits caps=limits;uint64_t value=axis==0?stats.allocated_bytes:axis==1?stats.peak_bytes:stats.work;value=delta<0?value-1:value+(unsigned)delta;
                if(!axis)caps.allocated_bytes=value;else if(axis==1)caps.live_bytes=value;else caps.work=value;CHECK(once(mode,&caps,SIZE_MAX,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
        printf("Folders mode=%u SourceFI=%zu appFI=%zu JSONallocations=0 full=%u atomic count/owners/revision physical0/0\n",mode,sites,app_sites,full?1u:0u);}
    lifetime(argv[1]);receiver_budget();return 0;
}
