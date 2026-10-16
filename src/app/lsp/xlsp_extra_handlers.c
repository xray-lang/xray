#include "xlsp_navigation.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_extra_handlers.c - Additional LSP handlers
 */

#include "xlsp_extra_handlers.h"
#include "xlsp_links.h"
#include "xlsp_source_index_store.h"
#include "xlsp_index_json.h"
#include "xlsp_hierarchy_request.h"
#include "../../api/xsource_compile.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

// Workspace-symbol collection over the shallow global index.
#define XLSP_WORKSPACE_SYMBOL_LIMIT 100

typedef struct {
    XrLspServer *server;
    XrJsonValue *symbols;
    const char *query;
    size_t query_length;
    int added;
} XlspWorkspaceSymbolCtx;
static XrXirStatus xlsp_workspace_symbol_collect(const XlspIndexedSymbol *view,void *context) {
    XlspWorkspaceSymbolCtx *ctx=context;
    if(ctx->added>=XLSP_WORKSPACE_SYMBOL_LIMIT)return XR_XIR_OK;
    XrLspDocument *open=xlsp_document_get(ctx->server,view->uri);
    if(open&&open->version!=view->version)return XR_XIR_OK;
    bool matches=false;XrXirStatus status=index_json_contains(ctx->server->index_resources,view->symbol->name,ctx->query,ctx->query_length,&matches);
    if(status!=XR_XIR_OK||!matches)return status;
    XrJsonValue *symbol=xjson_new_object(),*location=NULL;
    if(!symbol)return XR_XIR_OUT_OF_MEMORY;
    if(!index_json_member(symbol,"name",xjson_new_string(view->symbol->name))||
       !index_json_member(symbol,"kind",xjson_new_number(view->symbol->kind)))goto failed;
    location=xjson_new_object();if(!location)goto failed;
    if(!index_json_member(location,"uri",xjson_new_string(view->uri))||
       !index_json_member(location,"range",index_json_range(view->symbol->selection)))goto failed;
    {XrJsonValue *moved=location;location=NULL;if(!index_json_member(symbol,"location",moved))goto failed;}
    {int before=ctx->symbols->as.array.count;xjson_array_push(ctx->symbols,symbol);
     if(ctx->symbols->as.array.count!=before+1)goto failed;}
    ++ctx->added;return XR_XIR_OK;
failed:
    xjson_free(location);xjson_free(symbol);return XR_XIR_OUT_OF_MEMORY;
}

// ============================================================================
// Document Highlight
// ============================================================================

XrJsonValue *xlsp_handle_document_highlight(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 2);
}

// ============================================================================
// Workspace Symbol
// ============================================================================

XrJsonValue *xlsp_handle_workspace_symbol(XrLspServer *server,XrJsonValue *params) {
    if(server->index_failure!=XR_XIR_OK){server->source_failure.stage=2;server->source_failure.status=(unsigned)server->index_failure;return NULL;}
    XrJsonValue *query=xjson_get(params,"query"),*symbols=xjson_new_array();
    if(!symbols||!server->source_index){xjson_free(symbols);server->source_failure.stage=2;server->source_failure.status=XR_XIR_OUT_OF_MEMORY;return NULL;}
    XlspWorkspaceSymbolCtx context={server,symbols,"",0,0};
    if(query&&query->type==XR_JSON_STRING){context.query=query->as.string;context.query_length=query->string_len;}
    XrXirStatus status=xlsp_source_index_store_visit(server->source_index,xlsp_workspace_symbol_collect,&context);
    if(status!=XR_XIR_OK){xjson_free(symbols);server->source_failure.stage=2;server->source_failure.status=(unsigned)status;return NULL;}
    return symbols;
}

// ============================================================================
// Selection Range
// ============================================================================

XR_FUNC XrJsonValue *xlsp_handle_selection_range(XrLspServer *server,XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_SELECTION);
}

// ============================================================================
// Document Link
// ============================================================================

XrJsonValue *xlsp_handle_document_link(XrLspServer *server,XrJsonValue *params) {
    return xlsp_links_handle(server,params);
}
