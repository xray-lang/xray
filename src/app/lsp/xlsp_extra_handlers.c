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
#include "xlsp_ast_utils.h"
#include "xlsp_analysis.h"
#include "xlsp_imports.h"
#include "xlsp_source_index_store.h"
#include "xlsp_index_json.h"
#include "../../frontend/analyzer/xanalyzer.h"
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

XrJsonValue *xlsp_handle_selection_range(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *positions = xjson_get(params, "positions");
    if (!textDocument || !positions)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content)
        return xjson_new_array();

    XrJsonValue *ranges = xjson_new_array();

    for (int i = 0; i < xjson_array_len(positions); i++) {
        XrJsonValue *pos_obj = xjson_array_get(positions, i);
        XrLspPosition pos = {.line = (uint32_t) xjson_get_int(pos_obj, "line"),
                             .character = (uint32_t) xjson_get_int(pos_obj, "character")};

        uint32_t offset = xlsp_position_to_offset(doc, pos);
        const char *content = doc->content;

        uint32_t word_start = offset;
        uint32_t word_end = offset;

        while (word_start > 0 && xlsp_is_ident_char(content[word_start - 1])) {
            word_start--;
        }
        while (word_end < doc->length && xlsp_is_ident_char(content[word_end])) {
            word_end++;
        }

        uint32_t line_start = offset;
        uint32_t line_end = offset;
        while (line_start > 0 && content[line_start - 1] != '\n')
            line_start--;
        while (line_end < doc->length && content[line_end] != '\n')
            line_end++;

        // Build nested selection ranges: word -> line -> document
        XrJsonValue *doc_range = xjson_new_object();
        xjson_object_set(doc_range, "range", xjson_make_range(0, 0, doc->line_count - 1, 100));

        XrJsonValue *line_range = xjson_new_object();
        XrLspPosition ls = xlsp_offset_to_position(doc, line_start);
        XrLspPosition le = xlsp_offset_to_position(doc, line_end);
        xjson_object_set(line_range, "range",
                         xjson_make_range(ls.line, ls.character, le.line, le.character));
        xjson_object_set(line_range, "parent", doc_range);

        XrJsonValue *word_range = xjson_new_object();
        XrLspPosition ws = xlsp_offset_to_position(doc, word_start);
        XrLspPosition we = xlsp_offset_to_position(doc, word_end);
        xjson_object_set(word_range, "range",
                         xjson_make_range(ws.line, ws.character, we.line, we.character));
        xjson_object_set(word_range, "parent", line_range);

        xjson_array_push(ranges, word_range);
    }

    return ranges;
}

// ============================================================================
// Document Link
// ============================================================================

XrJsonValue *xlsp_handle_document_link(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc || !doc->content)
        return xjson_new_array();

    XrJsonValue *links = xjson_new_array();
    const char *content = doc->content;

    const char *p = content;
    while ((p = strstr(p, "import")) != NULL) {
        p += 6;

        while (*p == ' ' || *p == '\t')
            p++;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            const char *path_start = p;
            while (*p && *p != quote && *p != '\n')
                p++;

            if (*p == quote) {
                size_t path_len = p - path_start;
                char *path = xr_malloc(path_len + 1);
                memcpy(path, path_start, path_len);
                path[path_len] = '\0';

                char *resolved = xlsp_resolve_import_path(doc->uri, path);
                if (resolved) {
                    XrJsonValue *link = xjson_new_object();

                    size_t quote_offset = path_start - content - 1;
                    XrLspPosition link_start = xlsp_offset_to_position(doc, quote_offset);
                    XrLspPosition link_end =
                        xlsp_offset_to_position(doc, quote_offset + path_len + 2);

                    xjson_object_set(link, "range",
                                     xjson_make_range(link_start.line, link_start.character,
                                                      link_end.line, link_end.character));

                    char target_uri[512];
                    snprintf(target_uri, sizeof(target_uri), "file://%s", resolved);
                    xjson_object_set(link, "target", xjson_new_string(target_uri));

                    xjson_array_push(links, link);
                    xr_free(resolved);
                }
                xr_free(path);
            }
        }
    }

    return links;
}
