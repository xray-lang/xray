/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_analysis.c - Code analysis implementation
 */

#include "../../base/xresource_limits.h"
#include "xlsp_analysis.h"
#include "xlsp_source_open.h"
#include "../../api/xsource_compile.h"
#include "../../api/xsource_authority.h"
#include "xlsp_ast_utils.h"
#include "xlsp_cache.h"
#include "../../base/xfileio.h"
#include "../../base/xjson.h"
#include "xlsp_workspace.h"
#include "xlsp_utils.h"
#include <string.h>
#include <stdio.h>
#include "../../base/xmalloc.h"

// xray parser headers
#include "../../frontend/lexer/xlex.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../frontend/parser/xast_api.h"
#include "../../frontend/parser/xast_nodes.h"
#include "xlsp_stdlib.h"
#include "xlsp_imports.h"
#include "xlsp_index_pool.h"
#include "xlsp_source_index_store.h"

// Static analyzer
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/analyzer/xanalyzer_escape.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/value/xtype_pool.h"
#include "../../toolchain/xcompiler_session.h"
#include "../../module/xmodule_identity.h"
#include "../../module/xmodule_graph.h"
#include "../../runtime/xisolate_api.h"
#include "../../api/xsource_authority.h"

// AST formatter (lives in frontend/format).
#include "../../frontend/format/xfmt.h"
#include "../../frontend/analyzer/xanalyzer_ast_visitor.h"



char *xlsp_word_at_position(XrLspDocument *doc, XrLspPosition pos, uint32_t *out_start,
                            uint32_t *out_end) {
    if (!doc || !doc->content)
        return NULL;

    uint32_t offset = xlsp_position_to_offset(doc, pos);
    if (offset >= doc->length)
        return NULL;

    const char *content = doc->content;
    uint32_t start = offset;
    uint32_t end = offset;

    while (start > 0 && xlsp_is_ident_char(content[start - 1]))
        start--;
    while (end < doc->length && xlsp_is_ident_char(content[end]))
        end++;

    if (start == end)
        return NULL;

    size_t word_len = end - start;
    char *word = xr_malloc(word_len + 1);
    if (!word)
        return NULL;
    memcpy(word, content + start, word_len);
    word[word_len] = '\0';

    if (out_start)
        *out_start = start;
    if (out_end)
        *out_end = end;
    return word;
}

// Create a diagnostic JSON object
void xlsp_analysis_release_module_graph(XrLspServer *server) {
    if (!server)
        return;

    if (server->workspace_analyzer)
        xa_analyzer_set_graph(server->workspace_analyzer, NULL);

    /* Dependency ASTs are owned by the graph. Remove their analyzer scopes
     * before freeing it so symbol locations and names cannot outlive the AST
     * storage they borrow. The entry is analyzed from the open document AST
     * under its URI and therefore is not graph-owned. */
    if (server->module_graph && server->workspace_analyzer) {
        for (int i = 0; i < server->module_graph->spec_count; i++) {
            if (i == server->module_graph->entry_index)
                continue;
            const char *source_path = server->module_graph->specs[i].source_path;
            if (source_path)
                xa_analyzer_remove_file(server->workspace_analyzer, source_path);
        }
    }

    if (server->module_graph) {
        xr_compile_module_graph_free(server->module_graph);
        server->module_graph = NULL;
    }
    if (server->module_graph_authority) {
        xr_source_authority_close(server->module_graph_authority);
        xr_free(server->module_graph_authority);
        server->module_graph_authority = NULL;
    }
}

void xlsp_parse_document(XrLspDocument *doc,XrLspServer *server) {
    if(!doc||!server||!doc->uri||!doc->content)return;
    if(doc->source_open) {
        doc->ast=(AstNode *)xlsp_source_syntax_ast(doc->syntax_snapshot);
        doc->parse_error=xlsp_source_syntax_status(doc->syntax_snapshot)==XR_PARSE_RECOVERED||
            xlsp_source_open_query_status(doc->source_open)!=XR_XIR_OK;
    } else {
        XrCompileResourceLimits limits=(XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
        XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;XlspSyntaxSnapshot *fresh=NULL;
        if(xr_compile_resources_new(&limits,&resources)!=XR_COMPILE_RESOURCE_OK)return;
        XrCompileResourceStatus opened=xr_compile_session_new(resources,&session);
        XrParseStatus parsed=XR_PARSE_BAD_ARGUMENT;
        if(opened==XR_COMPILE_RESOURCE_OK) {
            XlspSourceDocument input={doc->uri,doc->content,strlen(doc->uri),doc->length,doc->version};
            parsed=xlsp_source_syntax_build(session,&input,&fresh);
        }
        xr_compile_session_free(session);xr_compile_resources_release(resources);
        if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED){xlsp_source_syntax_free(fresh);return;}
        XlspSyntaxSnapshot *old=doc->syntax_snapshot;
        doc->syntax_snapshot=fresh;doc->ast=(AstNode *)xlsp_source_syntax_ast(fresh);
        doc->parse_error=parsed==XR_PARSE_RECOVERED;xlsp_source_syntax_free(old);
    }
    xlsp_free_document_cache(doc);
    if(xlsp_source_syntax_diagnostics_json(doc->syntax_snapshot,(XrJsonValue **)&doc->cached_diagnostics)!=XR_XIR_OK)
        lsp_log("Syntax diagnostics unavailable; no empty success cached");
    if(doc->ast&&server->source_index) {
        XlspSourceIndex *index=NULL;XlspSourceDocument input={doc->uri,doc->content,strlen(doc->uri),doc->length,doc->version};
        XrXirStatus status=xlsp_source_index_build(server->index_resources,&input,&index);
        if(status==XR_XIR_OK)status=xlsp_source_index_store_replace(server->source_index,&index);
        xlsp_source_index_free(index);
        if(status!=XR_XIR_OK){xlsp_index_record_failure(server,status);lsp_log("Open document syntax index unavailable: %u",(unsigned)status);}
    }
    doc->dirty=false;
}
XrJsonValue *xlsp_analyze_diagnostics(XrLspDocument *doc) {
    if(!doc||!doc->syntax_snapshot)return NULL;
    XrJsonValue *out=NULL;
    XrXirStatus status=doc->source_open?
        xlsp_source_open_diagnostics_json(doc->source_open,doc->source_buffer,doc->syntax_snapshot,&out):
        xlsp_source_syntax_diagnostics_json(doc->syntax_snapshot,&out);
    if(status!=XR_XIR_OK)lsp_log("Source diagnostics unavailable: status %u",(unsigned)status);
    return out;
}

XrJsonValue *xlsp_analyze_document_symbols(XrLspDocument *doc) {
    XlspSourceOutline *outline=NULL;XrJsonValue *json=NULL;XrXirStatus status=XR_XIR_BAD_STAGE;
    if(doc&&doc->syntax_snapshot)status=xlsp_source_syntax_outline(doc->syntax_snapshot,&outline);
    if(status==XR_XIR_OK)status=xlsp_source_outline_json(outline,&json);
    xlsp_source_outline_free(outline);
    if(status!=XR_XIR_OK&&doc&&doc->server){doc->server->source_failure.stage=3;doc->server->source_failure.status=(unsigned)status;}
    return json;
}
