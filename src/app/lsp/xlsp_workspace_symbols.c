/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_workspace_symbols.c - Owned syntax display queries on independent ledgers
 */
#include "../../base/xresource_limits.h"
#include "xlsp_workspace_symbols.h"
#include "xlsp_source_params.h"
#include "xlsp_source_index_store.h"
#include "xlsp_document_store.h"
#include "xlsp_index_json.h"
#include "../../base/xchecks.h"
#include <string.h>

typedef struct WorkspaceDisplay {
    struct WorkspaceDisplay *next;
    char *uri,*name;
    size_t uri_length,name_length;
    XrLspRange selection;
    XrLspSymbolKind kind;
} WorkspaceDisplay;
struct XlspWorkspaceSymbolRequest {
    XrCompileResources *resources;
    XlspSourceParams *params;
    const XrJsonValue *query;
    WorkspaceDisplay *head,*tail;
    bool started;
};
typedef struct WorkspaceCapture {XlspWorkspaceSymbolRequest *request;const XrLspServer *server;} WorkspaceCapture;
enum { WORKSPACE_SYMBOL_LIMIT=100 };
static XrXirStatus workspace_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus workspace_work(XlspWorkspaceSymbolRequest *request,size_t units) {
    return workspace_resource(xr_compile_resources_work(request->resources,units));
}
XR_FUNC void xlsp_workspace_symbols_free(XlspWorkspaceSymbolRequest *request) {
    if(!request)return;
    XrCompileResources *resources=request->resources;WorkspaceDisplay *node=request->head;
    while(node){WorkspaceDisplay *next=node->next;xr_compile_resources_free(node->uri);xr_compile_resources_free(node->name);
        xr_compile_resources_free(node);node=next;}
    xlsp_source_params_free(request->params);xr_compile_resources_free(request);xr_compile_resources_release(resources);
}
static XrXirStatus workspace_string(XlspWorkspaceSymbolRequest *request,const char *input,char **out,size_t *size) {
    if(!input)return XR_XIR_BAD_STRUCTURE;
    size_t length=0;
    for(;;){XrXirStatus status=workspace_work(request,1);if(status!=XR_XIR_OK)return status;
        if(!input[length])break;if(length==SIZE_MAX-1)return XR_XIR_BUDGET;++length;}
    XrXirStatus status=workspace_resource(xr_compile_resources_alloc(request->resources,length+1,(void **)out));
    if(status==XR_XIR_OK)status=workspace_work(request,length+1);
    if(status==XR_XIR_OK){memcpy(*out,input,length+1);*size=length;}return status;
}
static XrXirStatus workspace_capture_symbol(const XlspIndexedSymbol *view,void *context) {
    WorkspaceCapture *capture=context;XlspWorkspaceSymbolRequest *request=capture->request;
    if(!view||!view->uri||!view->symbol||!view->symbol->name)return XR_XIR_BAD_STRUCTURE;
    WorkspaceDisplay *node=NULL;
    XrXirStatus status=workspace_resource(xr_compile_resources_calloc(request->resources,1,sizeof(*node),(void **)&node));
    if(status!=XR_XIR_OK)return status;
    if(request->tail)request->tail->next=node;else request->head=node;request->tail=node;
    status=workspace_string(request,view->uri,&node->uri,&node->uri_length);if(status!=XR_XIR_OK)return status;
    const XrLspDocument *open=xlsp_document_store_get(capture->server->doc_table,node->uri);
    if(open&&open->server!=capture->server)return XR_XIR_BAD_STRUCTURE;
    if(open&&open->version!=view->version){node->kind=0;return XR_XIR_OK;}
    status=workspace_string(request,view->symbol->name,&node->name,&node->name_length);
    if(status!=XR_XIR_OK)return status;
    node->kind=view->symbol->kind;node->selection=view->symbol->selection;
    return workspace_work(request,sizeof(*node));
}
static XrXirStatus workspace_query(XlspWorkspaceSymbolRequest *request,const XrJsonValue *params) {
    XrXirStatus status=workspace_resource(xlsp_source_params_new(request->resources,params,&request->params));
    if(status!=XR_XIR_OK)return status;
    const XrJsonValue *owned=xlsp_source_params_view(request->params);
    if(owned->type!=XR_JSON_OBJECT)return XR_XIR_BAD_STRUCTURE;
    for(int i=0;i<owned->as.object.count;++i) {
        status=workspace_work(request,sizeof(XrJsonMember));if(status!=XR_XIR_OK)return status;
        const XrJsonMember *member=&owned->as.object.members[i];if(member->key_len!=5)continue;
        status=workspace_work(request,5);if(status!=XR_XIR_OK)return status;
        if(!memcmp(member->key,"query",5)){if(request->query)return XR_XIR_BAD_STRUCTURE;request->query=member->value;}
    }
    const XrJsonValue *query=request->query;
    if(!query||query->type!=XR_JSON_STRING||!query->as.string)return XR_XIR_BAD_STRUCTURE;
    XlspSourceBuffer *buffer=NULL;status=xlsp_source_buffer_new(request->resources,query->as.string,query->string_len,0,&buffer);
    xlsp_source_buffer_free(buffer);return status;
}
XR_FUNC XrXirStatus xlsp_workspace_symbols_capture(XrLspServer *server,const XrJsonValue *params,XlspWorkspaceSymbolRequest **out) {
    if(!server||!out||*out)return XR_XIR_BAD_STRUCTURE;
    server->source_failure.stage=22;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    if(!params||server->shutdown_received||server->exit_received||!server->source_index)return XR_XIR_BAD_STRUCTURE;
    if(server->index_failure!=XR_XIR_OK){server->source_failure.status=(unsigned)server->index_failure;return server->index_failure;}
    const XrCompileResourceLimits limits=XR_RESOURCE_TOOL_COMPILER_LIMITS;
    XrCompileResources *resources=NULL;XlspWorkspaceSymbolRequest *request=NULL;
    XrXirStatus status=workspace_resource(xr_compile_resources_new(&limits,&resources));if(status!=XR_XIR_OK)goto done;
    status=workspace_resource(xr_compile_resources_calloc(resources,1,sizeof(*request),(void **)&request));
    if(status!=XR_XIR_OK)goto done;request->resources=resources;resources=NULL;
    status=workspace_query(request,params);
    WorkspaceCapture capture={request,server};
    if(status==XR_XIR_OK)status=xlsp_source_index_store_visit(server->source_index,request->resources,workspace_capture_symbol,&capture);
    if(status==XR_XIR_OK){*out=request;request=NULL;}
done:
    xlsp_workspace_symbols_free(request);xr_compile_resources_release(resources);
    server->source_failure.status=(unsigned)status;if(status==XR_XIR_OK)server->source_failure.stage=0;return status;
}
static XrJsonValue *workspace_symbol_json(const WorkspaceDisplay *node) {
    XrJsonValue *symbol=xjson_new_object(),*location=NULL;if(!symbol)return NULL;
    if(!index_json_member(symbol,"name",xjson_new_string_n(node->name,node->name_length))||
        !index_json_member(symbol,"kind",xjson_new_number(node->kind)))goto failed;
    location=xjson_new_object();if(!location)goto failed;
    if(!index_json_member(location,"uri",xjson_new_string_n(node->uri,node->uri_length))||
        !index_json_member(location,"range",index_json_range(node->selection)))goto failed;
    {XrJsonValue *moved=location;location=NULL;if(!index_json_member(symbol,"location",moved))goto failed;}
    return symbol;
failed:
    xjson_free(location);xjson_free(symbol);return NULL;
}
static XrXirStatus workspace_symbols_json(XlspWorkspaceSymbolRequest *request,XrJsonValue **out) {
    XrJsonValue *array=xjson_new_array();if(!array)return XR_XIR_OUT_OF_MEMORY;XrXirStatus status=XR_XIR_OK;
    for(const WorkspaceDisplay *node=request->head;node;node=node->next) {
        status=workspace_work(request,sizeof(*node));if(status!=XR_XIR_OK)goto done;
        if(!node->kind||array->as.array.count>=WORKSPACE_SYMBOL_LIMIT)continue;
        bool matches=false;status=index_json_contains(request->resources,node->name,request->query->as.string,request->query->string_len,&matches);
        if(status!=XR_XIR_OK)goto done;if(!matches)continue;
        status=workspace_work(request,node->name_length+node->uri_length);if(status!=XR_XIR_OK)goto done;
        XrJsonValue *symbol=workspace_symbol_json(node);if(!symbol){status=XR_XIR_OUT_OF_MEMORY;goto done;}
        int count=array->as.array.count;xjson_array_push(array,symbol);
        if(array->as.array.count!=count+1){xjson_free(symbol);status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    *out=array;array=NULL;
done:
    xjson_free(array);return status;
}
static bool workspace_cancel_poll(const void *context) {
    return atomic_load_explicit((const _Atomic bool *)context,memory_order_acquire);
}
XR_FUNC XrXirStatus xlsp_workspace_symbols_run(XlspWorkspaceSymbolRequest *request,const _Atomic bool *cancel,XlspSourceRequestResult *out) {
    if(!request||request->started||!out||out->json||out->token_update||out->stage||out->cancelled||out->status!=XR_XIR_OK)return XR_XIR_BAD_STRUCTURE;
    request->started=true;XlspSourceRequestResult result={.stage=22};bool bound=false;
    if(cancel&&workspace_cancel_poll((const void *)cancel)){result.cancelled=true;goto done;}
    if(cancel){result.status=workspace_resource(xr_compile_resources_begin_cancel(request->resources,workspace_cancel_poll,(const void *)cancel));
        if(result.status!=XR_XIR_OK)goto done;bound=true;}
    result.status=workspace_symbols_json(request,&result.json);if(result.status==XR_XIR_OK)result.stage=0;
done:
    if(bound){XrCompileResourceStatus ended=xr_compile_resources_end_cancel(request->resources,workspace_cancel_poll,(const void *)cancel);
        XR_CHECK(ended==XR_COMPILE_RESOURCE_OK||ended==XR_COMPILE_RESOURCE_CANCELLED,"Workspace query cancellation must retain its binding");
        if(ended==XR_COMPILE_RESOURCE_CANCELLED)result.cancelled=true;}
    if(cancel&&workspace_cancel_poll((const void *)cancel))result.cancelled=true;
    if(result.cancelled){xjson_free(result.json);result.json=NULL;}
    *out=result;return XR_XIR_OK;
}
XR_FUNC XrJsonValue *xlsp_workspace_symbols_handle(XrLspServer *server,const XrJsonValue *params) {
    if(!server)return NULL;
    uint64_t revision=server->source_revision,generation=server->doc_table?server->doc_table->generation:0;
    XlspWorkspaceSymbolRequest *request=NULL;XlspSourceRequestResult result={0};
    XrXirStatus status=xlsp_workspace_symbols_capture(server,params,&request);
    if(status==XR_XIR_OK)status=xlsp_workspace_symbols_run(request,NULL,&result);if(status==XR_XIR_OK)status=result.status;
    xlsp_workspace_symbols_free(request);
    if(status==XR_XIR_OK&&(server->shutdown_received||server->exit_received||server->source_revision!=revision||
        (server->doc_table?server->doc_table->generation:0)!=generation))status=XR_XIR_UNRESOLVED;
    if(status!=XR_XIR_OK){xjson_free(result.json);result.json=NULL;}
    server->source_failure.status=(unsigned)status;server->source_failure.stage=status==XR_XIR_OK?0u:22u;return result.json;
}
