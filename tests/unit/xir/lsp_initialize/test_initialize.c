/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_initialize.h"
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
#include "app/lsp/xlsp_semantic_tokens.c"
#include "app/lsp/xlsp_initialize_result.c"
#include "app/lsp/xlsp_source_initialize.c"
static const XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char *const cases[]={
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/init/a%20%CE%BB\",\"name\":\"A \\u03bb\\ud83d\\ude00\"},{\"uri\":\"file:///C:/init/b\",\"name\":\"B\"}]}",
    "{\"rootUri\":\"file:///C:/init/uri\"}",
    "{\"rootPath\":\"C:\\\\init\\\\path \\u03bb\\ud83d\\ude00\"}",
    "{\"rootUri\":\"file:///C:/init/uri\",\"rootPath\":\"C:/different\"}",
    "{\"rootUri\":null,\"rootPath\":\"C:/ignored\"}",
    "{\"workspaceFolders\":[],\"rootUri\":\"file:///C:/ignored\"}",
    "{\"workspaceFolders\":null,\"rootUri\":\"file:///C:/ignored\"}",
    "{}",
    "{\"rootUri\":null,\"rootPath\":null}",
    "{\"rootPath\":\"C:/init/long\"}",
    "{\"rootPath\":\"\\\\\\\\host\\\\share\\\\a b\"}",
    "{\"rootPath\":\"/tmp/init \\u03bb\\ud83d\\ude00\"}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/init/f0\",\"name\":\"N0\"},{\"uri\":\"file:///C:/init/f1\",\"name\":\"N1\"},{\"uri\":\"file:///C:/init/f2\",\"name\":\"N2\"},{\"uri\":\"file:///C:/init/f3\",\"name\":\"N3\"},{\"uri\":\"file:///C:/init/f4\",\"name\":\"N4\"},{\"uri\":\"file:///C:/init/f5\",\"name\":\"N5\"},{\"uri\":\"file:///C:/init/f6\",\"name\":\"N6\"},{\"uri\":\"file:///C:/init/f7\",\"name\":\"N7\"},{\"uri\":\"file:///C:/init/f8\",\"name\":\"N8\"},{\"uri\":\"file:///C:/init/f9\",\"name\":\"N9\"},{\"uri\":\"file:///C:/init/f10\",\"name\":\"N10\"},{\"uri\":\"file:///C:/init/f11\",\"name\":\"N11\"},{\"uri\":\"file:///C:/init/f12\",\"name\":\"N12\"},{\"uri\":\"file:///C:/init/f13\",\"name\":\"N13\"},{\"uri\":\"file:///C:/init/f14\",\"name\":\"N14\"},{\"uri\":\"file:///C:/init/f15\",\"name\":\"N15\"}]}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/init/f0\",\"name\":\"N0\"},{\"uri\":\"file:///C:/init/f1\",\"name\":\"N1\"},{\"uri\":\"file:///C:/init/f2\",\"name\":\"N2\"},{\"uri\":\"file:///C:/init/f3\",\"name\":\"N3\"},{\"uri\":\"file:///C:/init/f4\",\"name\":\"N4\"},{\"uri\":\"file:///C:/init/f5\",\"name\":\"N5\"},{\"uri\":\"file:///C:/init/f6\",\"name\":\"N6\"},{\"uri\":\"file:///C:/init/f7\",\"name\":\"N7\"},{\"uri\":\"file:///C:/init/f8\",\"name\":\"N8\"},{\"uri\":\"file:///C:/init/f9\",\"name\":\"N9\"},{\"uri\":\"file:///C:/init/f10\",\"name\":\"N10\"},{\"uri\":\"file:///C:/init/f11\",\"name\":\"N11\"},{\"uri\":\"file:///C:/init/f12\",\"name\":\"N12\"},{\"uri\":\"file:///C:/init/f13\",\"name\":\"N13\"},{\"uri\":\"file:///C:/init/f14\",\"name\":\"N14\"},{\"uri\":\"file:///C:/init/f15\",\"name\":\"N15\"},{\"uri\":\"file:///C:/init/f16\",\"name\":\"N16\"}]}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/init/a\",\"name\":\"A\"},{\"uri\":\"https://example.invalid/b\",\"name\":\"B\"}]}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/init/a\",\"name\":\"A\"},{\"uri\":\"file:///C:/init/a\",\"name\":\"B\"}]}",
    "{\"rootUri\":\"file:///C:/a\\u0000b\"}",
    "{\"rootPath\":\"C:/a\\u0000b\"}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/x\",\"name\":\"a\\u0000b\"}]}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/x\"}]}",
    "{\"workspaceFolders\":false}",
    "{\"workspaceFolders\":[],\"workspaceFolders\":[]}",
    "{\"rootUri\":null,\"rootUri\":\"file:///C:/x\"}",
    "{\"rootPath\":\"C:/x\",\"rootPath\":\"C:/y\"}",
    "{\"workspaceFolders\\u0000bad\":[]}",
    "{\"rootPath\":\"C:/invalid\"}",
    "{\"rootPath\":\"relative/path\"}",
    "{\"rootUri\":\"https://example.invalid/a\"}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/x\",\"uri\":\"file:///C:/y\",\"name\":\"X\"}]}",
    "{\"workspaceFolders\":[],\"rootPath\":42}",
    "{\"rootUri\":\"file:///C:/invalid\"}",
    "{\"workspaceFolders\":[{\"uri\":\"file:///C:/x\",\"name\":\"X\",\"name\":\"Y\"}]}",
    "{\"capabilities\":false}",
    "{\"capabilities\":{\"textDocument\":false}}",
    "{\"capabilities\":{\"textDocument\":{\"rename\":false}}}",
    "{\"capabilities\":{\"textDocument\":{\"rename\":{\"prepareSupport\":1}}}}",
    "{\"capabilities\":{\"textDocument\":{\"codeAction\":{\"codeActionLiteralSupport\":true}}}}",
    "{\"capabilities\":{\"textDocument\":{\"codeAction\":{\"codeActionLiteralSupport\":{}}}}}",
    "{\"capabilities\":{\"textDocument\":{\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":false}}}}}}",
    "{\"capabilities\":{\"textDocument\":{\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":[\"a\\u0000b\"]}}}}}}",
    "{\"capabilities\":{\"textDocument\":{\"rename\":{\"prepareSupport\":true,\"prepareSupport\":false}}}}",
    "{}",
    "{\"capabilities\":{\"textDocument\":{\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":[]}}}}}}",
    "{\"capabilities\":{\"textDocument\":{\"rename\":{\"prepareSupport\":true}}}}",
    "{\"capabilities\":{},\"capabilities\":{}}",
    "{\"capabilities\":{\"textDocument\":{\"rename\":{\"bad\\u0000key\":null}}}}",
};
static unsigned flags(unsigned mode){return mode%3==0?127u:mode%3==1?0u:53u;}
static void capabilities(XrLspServer *server,unsigned bits){
    server->capabilities.completion=(bits&1)!=0;server->capabilities.hover=(bits&2)!=0;
    server->capabilities.definition=(bits&4)!=0;server->capabilities.references=(bits&8)!=0;
    server->capabilities.document_symbol=(bits&16)!=0;server->capabilities.formatting=(bits&32)!=0;
    server->capabilities.rename=(bits&64)!=0;
}
static XrXirStatus expected(unsigned mode){return mode<13||mode==42||mode==43?XR_XIR_OK:mode==13?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;}
static unsigned client(unsigned mode){return mode==42?2:mode==43?1:(mode%2?0:3);}
static unsigned folders(unsigned mode){return mode==0?2:mode==12?16:(mode==1||mode==2||mode==3||mode==9||mode==10||mode==11)?1:0;}
static void boolean(XrJsonValue *object,const char *key){XrJsonValue *v=xjson_get(object,key);CHECK(v&&v->type==XR_JSON_BOOL&&v->as.boolean);}
static void strings(XrJsonValue *object,const char *key,const char *const *want,size_t count){
    XrJsonValue *array=xjson_get(object,key);CHECK(array&&array->type==XR_JSON_ARRAY&&array->as.array.count==(int)count);
    for(size_t i=0;i<count;++i){XrJsonValue *v=xjson_array_get(array,(int)i);CHECK(v&&v->type==XR_JSON_STRING&&!strcmp(v->as.string,want[i]));}
}
static void response(XrJsonValue *result,unsigned bits,unsigned options){
    CHECK(result&&result->type==XR_JSON_OBJECT&&result->as.object.count==2);XrJsonValue *cap=xjson_get(result,"capabilities"),*info=xjson_get(result,"serverInfo");
    CHECK(cap&&cap->type==XR_JSON_OBJECT&&info&&info->type==XR_JSON_OBJECT&&info->as.object.count==2);
    CHECK(!strcmp(xjson_get_string(info,"name"),"xray-lsp")&&!strcmp(xjson_get_string(info,"version"),XRAY_VERSION_STRING));
    const char *keys[]={"completionProvider","hoverProvider","definitionProvider","referencesProvider","documentSymbolProvider","documentFormattingProvider","renameProvider"};unsigned conditional=0;
    for(unsigned i=0;i<7;++i){CHECK((xjson_get(cap,keys[i])!=NULL)==((bits&(1u<<i))!=0));if(bits&(1u<<i))conditional+=i==5?2:1;}
    CHECK(cap->as.object.count==16+(int)conditional);XrJsonValue *sync=xjson_get_object(cap,"textDocumentSync");CHECK(sync&&sync->as.object.count==2);boolean(sync,"openClose");CHECK(xjson_get_int(sync,"change")==2);
    if(bits&1){XrJsonValue *v=xjson_get_object(cap,"completionProvider");const char *want[]={"."};strings(v,"triggerCharacters",want,1);boolean(v,"resolveProvider");}
    if(bits&32){XrJsonValue *v=xjson_get_object(cap,"documentOnTypeFormattingProvider");const char *want[]={"\n",";"};CHECK(!strcmp(xjson_get_string(v,"firstTriggerCharacter"),"}"));strings(v,"moreTriggerCharacter",want,2);}
    if(bits&64){if(options&1)boolean(xjson_get_object(cap,"renameProvider"),"prepareProvider");else boolean(cap,"renameProvider");}
    const char *sig[]={"(",","},*actions[]={"quickfix","source.organizeImports"};strings(xjson_get_object(cap,"signatureHelpProvider"),"triggerCharacters",sig,2);if(options&2)strings(xjson_get_object(cap,"codeActionProvider"),"codeActionKinds",actions,2);else boolean(cap,"codeActionProvider");
    const char *truths[]={"inlayHintProvider","foldingRangeProvider","documentHighlightProvider","workspaceSymbolProvider","selectionRangeProvider","callHierarchyProvider","typeHierarchyProvider","implementationProvider"};
    for(size_t i=0;i<sizeof(truths)/sizeof(*truths);++i)boolean(cap,truths[i]);
    CHECK(!strcmp(xjson_get_string(cap,"positionEncoding"),"utf-16"));
    XrJsonValue *links=xjson_get_object(cap,"documentLinkProvider"),*lens=xjson_get_object(cap,"codeLensProvider");CHECK(links&&!links->as.object.count&&lens&&!lens->as.object.count);
    XrJsonValue *roots=xjson_get_object(xjson_get_object(cap,"workspace"),"workspaceFolders");boolean(roots,"supported");boolean(roots,"changeNotifications");
    XrJsonValue *semantic=xjson_get_object(cap,"semanticTokensProvider"),*legend=xjson_get_object(semantic,"legend");CHECK(semantic&&semantic->as.object.count==3&&legend);
    boolean(semantic,"range");boolean(xjson_get_object(semantic,"full"),"delta");
    const char *types[]={"namespace","type","class","enum","interface","struct","typeParameter","parameter","variable","property","enumMember","event","function","method","macro","keyword","modifier","comment","string","number","regexp","operator"};
    const char *modifiers[]={"declaration","definition","readonly","static","deprecated","async","modification","documentation","defaultLibrary"};
    strings(legend,"tokenTypes",types,sizeof(types)/sizeof(*types));strings(legend,"tokenModifiers",modifiers,sizeof(modifiers)/sizeof(*modifiers));
}
static XrJsonValue *input(unsigned mode){
    XrJsonValue *json=xjson_parse(cases[mode],strlen(cases[mode]));CHECK(json);
    if(!xjson_get(json,"capabilities")&&mode!=41){const char *basic="{}",*supported="{\"textDocument\":{\"rename\":{\"prepareSupport\":true},\"codeAction\":{\"codeActionLiteralSupport\":{\"codeActionKind\":{\"valueSet\":[\"quickfix\",\"source.organizeImports\"]}}}}}";
        const char *data=client(mode)?supported:basic;XrJsonValue *cap=xjson_parse(data,strlen(data));CHECK(cap);xjson_object_set(json,"capabilities",cap);}
    if(mode==9){char path[5024];memcpy(path,"C:/init/",8);memset(path+8,'x',5000);path[5008]=0;xjson_object_set(json,"rootPath",xjson_new_string(path));}
    if(mode==25||mode==30){XrJsonValue *v=xjson_get(json,mode==25?"rootPath":"rootUri");((unsigned char *)v->as.string)[0]=0xff;}
    return json;
}
static void cleanup(XrLspServer *server){
    for(int i=0;i<server->workspace_folder_count;++i)folders_release(&server->workspace_folders[i],true);
    xlsp_source_index_store_free(server->source_index);
}
static XrXirStatus once(unsigned mode,const XrCompileResourceLimits *caps,size_t fail,size_t json_fail,size_t app_fail,XrCompileResourceStats *stats){
    XrLspServer server={0};capabilities(&server,flags(mode));server.source_revision=10;XrJsonValue *params=input(mode);
    runtime_attempts=0;runtime_fail_at=fail;app_attempts=0;app_fail_at=app_fail;lsp_json_calls=0;lsp_json_fail=json_fail;lsp_json_injected=false;
    XrCompileResources *r=NULL;XrCompileResourceStatus opened=xr_compile_resources_new(caps,&r);
    XrXirStatus status=opened==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:opened==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    server.index_resources=r;if(status==XR_XIR_OK)status=xlsp_source_index_store_new(r,&server.source_index);XrLspServer before=server;
    XlspSourceInitialize *owner=NULL;XrJsonValue *result=NULL;
    if(status==XR_XIR_OK)status=xlsp_source_initialize_prepare(r,&server,params,&owner);xjson_free(params);
    if(status==XR_XIR_OK)status=xlsp_source_initialize_commit(owner,&server,&result);
    if(status!=XR_XIR_OK){CHECK(!result&&!memcmp(&before,&server,sizeof(server)));}
    else{CHECK(server.initialized&&server.workspace_folder_count==(int)folders(mode)&&server.source_revision==(folders(mode)?11u:10u));
        for(int i=0;i<server.workspace_folder_count;++i){CHECK(server.workspace_folders[i].uri&&server.workspace_folders[i].name&&server.workspace_folders[i].path&&server.workspace_folders[i].index_requested&&!server.workspace_folders[i].config_loaded&&!server.workspace_folders[i].index_completed);}
        if(mode==0)CHECK(!strcmp(server.workspace_folders[0].name,"A λ😀")&&strstr(server.workspace_folders[0].path,"a λ"));
        if(mode==1||mode==3)CHECK(!strcmp(server.workspace_folders[0].uri,"file:///C:/init/uri")&&!strcmp(server.workspace_folders[0].path,"C:/init/uri"));
        if(mode==2)CHECK(!strcmp(server.workspace_folders[0].uri,"file:///C:/init/path%20%CE%BB%F0%9F%98%80")&&!strcmp(server.workspace_folders[0].path,"C:/init/path λ😀"));
        if(mode==9)CHECK(strlen(server.workspace_folders[0].path)==5008&&strlen(server.workspace_folders[0].uri)>5000);
        if(mode==10)CHECK(!strcmp(server.workspace_folders[0].uri,"file://host/share/a%20b")&&!strcmp(server.workspace_folders[0].path,"//host/share/a b"));
        if(mode==11)CHECK(!strcmp(server.workspace_folders[0].path,"/tmp/init λ😀"));
    }
    if(stats&&r)*stats=sdk_stats(r);xlsp_source_initialize_free(owner);cleanup(&server);xr_compile_resources_release(r);
    CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes);if(result)response(result,flags(mode),client(mode));xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);
    runtime_fail_at=app_fail_at=lsp_json_fail=SIZE_MAX;return status;
}
static void lifetime(void){
    XrLspServer server={0};capabilities(&server,127);XrCompileResources *r=sdk_ledger(&limits);server.index_resources=r;CHECK(xlsp_source_index_store_new(r,&server.source_index)==XR_XIR_OK);
    XrJsonValue *params=input(0);XlspSourceInitialize *owner=NULL;CHECK(xlsp_source_initialize_prepare(r,&server,params,&owner)==XR_XIR_OK);
    XlspSourceInitialize *occupied=owner;size_t calls=runtime_attempts;CHECK(xlsp_source_initialize_prepare(r,&server,params,&occupied)==XR_XIR_BAD_STRUCTURE&&occupied==owner&&runtime_attempts==calls);
    xjson_free(params);xr_compile_resources_release(r);XrLspServer before=server;XrJsonValue *result=NULL;size_t app_calls=app_attempts;
    server.capabilities.rename=false;CHECK(xlsp_source_initialize_commit(owner,&server,&result)==XR_XIR_UNRESOLVED&&!result&&app_attempts==app_calls);server=before;
    server.source_revision=1;CHECK(xlsp_source_initialize_commit(owner,&server,&result)==XR_XIR_UNRESOLVED&&!result&&app_attempts==app_calls);server=before;
    result=xjson_new_null();XrJsonValue *kept=result;CHECK(xlsp_source_initialize_commit(owner,&server,&result)==XR_XIR_BAD_STRUCTURE&&result==kept);xjson_free(result);result=NULL;
    CHECK(xlsp_source_initialize_commit(owner,&server,&result)==XR_XIR_OK);occupied=NULL;
    CHECK(xlsp_source_initialize_prepare(r,&server,NULL,&occupied)==XR_XIR_BAD_STAGE&&!occupied);kept=result;result=NULL;
    CHECK(xlsp_source_initialize_commit(owner,&server,&result)==XR_XIR_BAD_STAGE&&!result);
    xlsp_source_initialize_free(owner);cleanup(&server);CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes);response(kept,127,3);xjson_free(kept);CHECK(!lsp_json_live&&!lsp_json_bytes);
    for(unsigned mode=1;mode<=2;++mode){server=(XrLspServer){0};r=sdk_ledger(&limits);server.index_resources=r;CHECK(xlsp_source_index_store_new(r,&server.source_index)==XR_XIR_OK);server.source_revision=UINT64_MAX;params=input(mode==1?1:7);owner=NULL;
        CHECK(xlsp_source_initialize_prepare(r,&server,params,&owner)==XR_XIR_OK);xjson_free(params);before=server;result=NULL;
        CHECK(xlsp_source_initialize_commit(owner,&server,&result)==(mode==1?XR_XIR_BUDGET:XR_XIR_OK));if(mode==1)CHECK(!result&&!memcmp(&server,&before,sizeof(server)));else CHECK(result&&server.initialized&&server.source_revision==UINT64_MAX);
        xlsp_source_initialize_free(owner);cleanup(&server);xr_compile_resources_release(r);xjson_free(result);CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes&&!lsp_json_live&&!lsp_json_bytes);}
}
static void admission(void){
    XrLspServer server={0};XrCompileResources *r=sdk_ledger(&limits);XlspSourceInitialize *owner=NULL;size_t calls=runtime_attempts;
    CHECK(xlsp_source_initialize_prepare(r,&server,NULL,&owner)==XR_XIR_BAD_STRUCTURE&&!owner&&runtime_attempts==calls);
    server.index_resources=r;CHECK(xlsp_source_index_store_new(r,&server.source_index)==XR_XIR_OK);calls=runtime_attempts;
    server.shutdown_received=true;CHECK(xlsp_source_initialize_prepare(r,&server,NULL,&owner)==XR_XIR_BAD_STAGE&&!owner&&runtime_attempts==calls);
    server.shutdown_received=false;server.exit_received=true;CHECK(xlsp_source_initialize_prepare(r,&server,NULL,&owner)==XR_XIR_BAD_STAGE&&!owner&&runtime_attempts==calls);server.exit_received=false;
    XrCompileResourceLimits closed={sizeof(XrCompileResources),sizeof(XrCompileResources),UINT64_MAX};XrCompileResources *receiver=sdk_ledger(&closed);server.index_resources=receiver;
    XrJsonValue *params=input(1);CHECK(xlsp_source_initialize_prepare(r,&server,params,&owner)==XR_XIR_OK);xjson_free(params);
    XrLspServer before=server;XrJsonValue *result=NULL;size_t live=app_live,bytes=app_bytes;
    CHECK(xlsp_source_initialize_commit(owner,&server,&result)==XR_XIR_BUDGET&&!result&&!memcmp(&before,&server,sizeof(server))&&app_live==live&&app_bytes==bytes);
    xlsp_source_initialize_free(owner);cleanup(&server);xr_compile_resources_release(receiver);xr_compile_resources_release(r);
    CHECK(!runtime_live&&!runtime_bytes&&!app_live&&!app_bytes&&!lsp_json_live&&!lsp_json_bytes);
}
int main(void){
    (void)sdk_unlimited;(void)app_strdup;bool full=getenv("XR_LSP_INITIALIZE_NORMAL_ONLY")==NULL;
    for(unsigned mode=0;mode<sizeof(cases)/sizeof(*cases);++mode){XrCompileResourceStats stats={0};CHECK(once(mode,&limits,SIZE_MAX,SIZE_MAX,SIZE_MAX,&stats)==expected(mode));size_t source=runtime_attempts,json=lsp_json_calls,app=app_attempts;
        if(full){for(size_t i=0;i<source;++i)CHECK(once(mode,&limits,i,SIZE_MAX,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY);
            for(size_t i=0;i<json;++i){CHECK(once(mode,&limits,SIZE_MAX,i,SIZE_MAX,NULL)==XR_XIR_OUT_OF_MEMORY);CHECK(lsp_json_injected);}
            for(size_t i=0;i<app;++i)CHECK(once(mode,&limits,SIZE_MAX,SIZE_MAX,i,NULL)==XR_XIR_OUT_OF_MEMORY);
            if(expected(mode)==XR_XIR_OK)for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta){XrCompileResourceLimits caps=limits;uint64_t value=axis==0?stats.allocated_bytes:axis==1?stats.peak_bytes:stats.work;value=delta<0?value-1:value+(unsigned)delta;
                if(!axis)caps.allocated_bytes=value;else if(axis==1)caps.live_bytes=value;else caps.work=value;CHECK(once(mode,&caps,SIZE_MAX,SIZE_MAX,SIZE_MAX,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));}}
        printf("Initialize mode=%u SourceFI=%zu JSONFI=%zu appFI=%zu full=%u atomic response/roots/initialized physical0/0\n",mode,source,json,app,full?1u:0u);}
    for(unsigned bits=0;bits<128;++bits)for(unsigned choice=0;choice<4;++choice){XrLspServer server={0};capabilities(&server,bits);XlspInitializeOptions options={(choice&1)!=0,(choice&2)!=0};XrJsonValue *result=xlsp_initialize_result(&server,&options);response(result,bits,choice);xjson_free(result);CHECK(!lsp_json_live&&!lsp_json_bytes);}
    lifetime();admission();return 0;
}
