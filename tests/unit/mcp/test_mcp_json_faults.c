/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mcp_json_faults.c - Every MCP heap failure with genuine production handlers
 */
#include "app/mcp/xmcp_json.h"
#include "app/mcp/xmcp_source.h"
#include "app/mcp/xmcp_server.h"
#include "app/mcp/xmcp_protocol.h"
#include "app/mcp/xmcp_tools.h"
#include "app/mcp/xmcp_tools_internal.h"
#include "app/mcp/xmcp_resources.h"
#include "app/mcp/xmcp_prompts.h"
#include "base/xmalloc.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation owned[32768];
static size_t attempts, fail_at=SIZE_MAX, live, physical_bytes;
static bool injected;
static size_t find_pointer(void *pointer) {
    for(size_t i=0;i<32768;++i) if(owned[i].pointer==pointer) return i;
    CHECK(false);return 0;
}
static void record(void *pointer,size_t bytes) {
    if(!pointer)return;
    for(size_t i=0;i<32768;++i) if(!owned[i].pointer) {
        owned[i]=(Allocation){pointer,bytes};++live;physical_bytes+=bytes;return;
    }
    CHECK(false);
}
static bool fail(void) { if(attempts++==fail_at){injected=true;return true;}return false; }
void *mcp_fault_malloc(size_t size) { if(fail())return NULL;void *p=xr_malloc(size);record(p,size);return p; }
void *mcp_fault_calloc(size_t count,size_t size) {
    if(fail())return NULL;CHECK(!size || count<=SIZE_MAX/size);
    void *p=xr_calloc(count,size);record(p,count*size);return p;
}
void *mcp_fault_realloc(void *pointer,size_t bytes) {
    if(fail())return NULL;
    size_t index=pointer?find_pointer(pointer):SIZE_MAX;
    void *p=xr_realloc(pointer,bytes);
    if(p){if(pointer){physical_bytes-=owned[index].bytes;owned[index]=(Allocation){p,bytes};physical_bytes+=bytes;}
        else record(p,bytes);}
    return p;
}
void mcp_fault_free(void *pointer) {
    if(pointer){size_t index=find_pointer(pointer);physical_bytes-=owned[index].bytes;owned[index]=(Allocation){0};--live;}
    xr_free(pointer);
}
char *mcp_fault_strdup(const char *text) {
    size_t size=strlen(text)+1;char *p=mcp_fault_malloc(size);if(p)memcpy(p,text,size);return p;
}
static XrJsonValue *parameters(unsigned kind) {
    XrJsonValue *p=xjson_new_object();CHECK(p);
    if(kind==2 || kind==3 || kind==4 || kind==5 || kind==6) {
        const char *tool=kind==2?"xray_analyze":kind==3?"xray_format":kind==4?"xray_definition":kind==5?"xray_run":"xray_analyze";
        XJSON_SET_STRING(p,"name",tool);XrJsonValue *args=xjson_new_object();
        XJSON_SET_STRING(args,"code",kind==6?"var x = ;\n":"export fn answer()->i64 { return 42 }\nprint(42)\n");
        XJSON_SET_STRING(args,"moduleId","mcp-json-faults");
        if(kind==4)XJSON_SET_STRING(args,"symbol","answer");
        xjson_object_set(p,"arguments",args);
        if(kind==6){XrJsonValue *meta=xjson_new_object();XJSON_SET_STRING(meta,"progressToken","owned-token");xjson_object_set(p,"_meta",meta);}
    } else if(kind==8)XJSON_SET_STRING(p,"uri","xray://spec/cheatsheet");
    else if(kind==10){XJSON_SET_STRING(p,"name","code-review");XrJsonValue *a=xjson_new_object();XJSON_SET_STRING(a,"code","print(42)\n");xjson_object_set(p,"arguments",a);}
    return p;
}
static XrJsonValue *invoke(unsigned kind,XmcpServer *server,XrJsonValue *p,XmcpRpcError *error) {
    switch(kind){
    case 0:return xmcp_handle_initialize(server,p,error);
    case 1:return xmcp_handle_tools_list(server,p,error);
    case 2:case 3:case 4:case 5:case 6:return xmcp_handle_tools_call(server,p,error);
    case 7:return xmcp_handle_resources_list(server,error);
    case 8:return xmcp_handle_resources_read(server,p,error);
    case 9:return xmcp_handle_prompts_list(server,error);
    default:return xmcp_handle_prompts_get(server,p,error);
    }
}
static void complete_case(unsigned kind) {
    size_t sites=0;
    for(size_t probe=0;probe<=sites;++probe) {
        xmcp_json_begin();XmcpServerOptions options;xmcp_server_options_default(&options);options.enable_runner=true;
        XmcpServer *server=xmcp_server_new(&options);CHECK(server);
        XrJsonValue *p=parameters(kind);XmcpRpcError error={0};
        attempts=0;injected=false;fail_at=probe?probe-1:SIZE_MAX;
        XrJsonValue *result=invoke(kind,server,p,&error);
        if(!probe){CHECK(result && !xmcp_json_failed());sites=attempts;CHECK(sites);}
        else {
            CHECK(injected);
            CHECK(!result || xjson_get_bool(result,"isError"));
            if(result)CHECK(xjson_get_int_or(xjson_get_object(result,"structuredContent"),"sourceStatus",-1)==XR_XIR_OUT_OF_MEMORY);
        }
        fail_at=SIZE_MAX;xjson_free(result);xjson_free(p);xmcp_server_free(server);
        CHECK(!live && !physical_bytes);
        xmcp_json_begin();
    }
    printf("MCP handler%u: %zu complete heap faults; no partial response; physical0\n",kind,sites);
}
int main(void) { xr_test_suppress_dialogs();for(unsigned kind=0;kind<11;++kind)complete_case(kind);return 0; }
