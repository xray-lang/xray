#include "xlsp_navigation.h"
/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_handlers_textdoc.c - LSP text document handlers
 *   didOpen, didChange, didClose, completion, hover, definition,
 *   references, rename, formatting, semantic tokens, inlay hints, etc.
 */

#include "xlsp_handlers_textdoc.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
#include "xlsp_analysis.h"
#include "xlsp_format.h"
#include "xlsp_on_type.h"
#include "xlsp_workspace.h"
#include "xlsp_rename.h"
#include "xlsp_semantic_tokens.h"
#include "xlsp_inlay_hints.h"
#include "xlsp_utils.h"
#include "../../frontend/analyzer/xanalyzer.h"
#include "../../frontend/parser/xast_nodes.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

void xlsp_handle_td_did_open(XrLspServer *server,XrJsonValue *params) {
    if(!server)return;
    XrJsonValue *document=xjson_get_object(params,"textDocument");
    XrJsonValue *uri=xjson_get(document,"uri"),*text=xjson_get(document,"text"),*version=xjson_get(document,"version");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||!text||text->type!=XR_JSON_STRING||!text->as.string||
        !version||version->type!=XR_JSON_NUMBER||!version->is_integer||version->as.integer<INT32_MIN||version->as.integer>INT32_MAX) {
        lsp_log("didOpen rejected: invalid bounded document input");return;
    }
    XlspSourceDocument input={uri->as.string,text->as.string,uri->string_len,text->string_len,version->as.integer};
    XrLspDocument *doc=xlsp_document_open(server,&input);
    if(!doc){lsp_log("didOpen failed; document table unchanged");return;}
    if(server->config.diagnostics_enabled)xlsp_publish_diagnostics(server,doc);
}

void xlsp_handle_td_did_change(XrLspServer *server,XrJsonValue *params) {
    if(!server)return;
    XrJsonValue *document=xjson_get_object(params,"textDocument");
    XrJsonValue *uri=xjson_get(document,"uri"),*version=xjson_get(document,"version");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len)||
        !version||version->type!=XR_JSON_NUMBER||!version->is_integer||version->as.integer<INT32_MIN||version->as.integer>INT32_MAX) {
        lsp_log("didChange rejected: invalid URI or version");return;
    }
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    if(!doc)return;
    XrXirStatus status=xlsp_document_apply_changes(doc,xjson_get(params,"contentChanges"),(int)version->as.integer);
    if(status!=XR_XIR_OK) {lsp_log("didChange rejected: status %u, previous version retained",(unsigned)status);return;}
    const char *uri_text=uri->as.string;
    (void)uri_text;
    /* Syntax and the one Source pipeline already ran before commit. */
    xlsp_schedule_diagnostics(server,doc);
}

void xlsp_handle_td_did_close(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    if (!textDocument)
        return;

    const char *uri = xjson_get_string(textDocument, "uri");
    if (uri) {
        xlsp_document_close(server, uri);
        xlsp_workspace_index_file(server,uri,NULL);
        // The closed document and obsolete Source workspace owners are gone.
    }
}

XrJsonValue *xlsp_handle_td_completion(XrLspServer *server, XrJsonValue *params) {
    lsp_log("handle_completion: CALLED");
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position) {
        lsp_log("handle_completion: missing textDocument or position");
        return xjson_new_null();
    }

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    lsp_log("handle_completion: uri=%s, line=%d, char=%d", uri, pos.line, pos.character);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);

    int item_count = items ? xjson_array_len(items) : 0;
    int max_items = server->config.completion_max_items;
    bool truncated = (max_items > 0 && item_count > max_items);
    if (truncated) {
        xjson_array_truncate(items, max_items);
        item_count = max_items;
    }
    lsp_log("handle_completion: returning %d items%s", item_count, truncated ? " (truncated)" : "");

    XrJsonValue *result = xjson_new_object();
    xjson_object_set(result, "isIncomplete", xjson_new_bool(truncated));
    xjson_object_set(result, "items", items);

    return result;
}

// Completion resolve: add detailed documentation from analyzer
XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *data = xjson_get_object(params, "data");
    if (!data)
        return xjson_clone(params);

    const char *uri = xjson_get_string(data, "uri");
    const char *name = xjson_get_string(params, "label");

    XrJsonValue *result = xjson_clone(params);
    if (!name)
        return result;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    char doc_str[XLSP_MAX_PATH];
    int len = 0;
    bool resolved = false;

    // Try analyzer for real type information
    if (analyzer) {
        XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
        if (sym) {
            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);

            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                len = snprintf(doc_str, sizeof(doc_str), "```xray\nfn %s(", name);
                if (links && links->param_count > 0) {
                    for (int i = 0; i < links->param_count; i++) {
                        if (i > 0)
                            len += snprintf(doc_str + len, sizeof(doc_str) - len, ", ");
                        const char *pname = (links->param_names && links->param_names[i])
                                                ? links->param_names[i]
                                                : "_";
                        XrType *ptype_obj = (links->param_types && links->param_types[i])
                                                ? links->param_types[i]
                                                : NULL;
                        len = xlsp_append_param_display(doc_str, sizeof(doc_str), len, pname,
                                                        ptype_obj,
                                                        xlsp_function_param_mode(links->type, i));
                    }
                }
                const char *ret =
                    (links && links->return_type) ? xr_type_to_string(links->return_type) : "void";
                snprintf(doc_str + len, sizeof(doc_str) - len, "): %s\n```", ret);
                resolved = true;
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(doc_str, sizeof(doc_str), "```xray\nclass %s\n```", name);
                resolved = true;
            } else {
                XrType *type = xa_analyzer_get_type(analyzer, sym);
                const char *type_str = type ? xr_type_to_string(type) : "<error>";
                const char *kw = sym->is_const ? "const" : "var";
                snprintf(doc_str, sizeof(doc_str), "```xray\n%s %s: %s\n```", kw, name, type_str);
                resolved = true;
            }
        }
    }

    // Fallback: generic documentation from symbol table
    if (!resolved && uri) {
        XrLspDocument *doc = xlsp_document_get(server, uri);
        if (doc && doc->ast) {
            snprintf(doc_str, sizeof(doc_str), "Symbol `%s` defined in this module.", name);
            resolved = true;
        }
    }

    if (resolved) {
        XrJsonValue *documentation = xjson_new_object();
        xjson_object_set(documentation, "kind", xjson_new_string("markdown"));
        xjson_object_set(documentation, "value", xjson_new_string(doc_str));
        xjson_object_set(result, "documentation", documentation);
    }

    return result;
}

XrJsonValue *xlsp_handle_td_hover(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 3);
}

XrJsonValue *xlsp_handle_td_document_symbol(XrLspServer *server, XrJsonValue *params) {
    if(!server)return NULL;
    server->source_failure.stage=10;server->source_failure.status=XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document=xjson_get_object(params,"textDocument"),*uri=xjson_get(document,"uri");
    if(!uri||uri->type!=XR_JSON_STRING||!uri->as.string||memchr(uri->as.string,0,uri->string_len))return NULL;
    XrLspDocument *doc=xlsp_document_get(server,uri->as.string);
    if(!doc||doc->server!=server||!doc->source_open||!doc->syntax_snapshot) {
        server->source_failure.status=XR_XIR_UNRESOLVED;return NULL;
    }
    XlspSourceOutline *outline=NULL;XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_syntax_outline(doc->syntax_snapshot,&outline);
    if(status==XR_XIR_OK)status=xlsp_source_outline_json(outline,&result);
    xlsp_source_outline_free(outline);server->source_failure.status=(unsigned)status;
    if(status==XR_XIR_OK)server->source_failure.stage=0;
    return result;
}

XrJsonValue *xlsp_handle_td_definition(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 0);
}

XrJsonValue *xlsp_handle_td_references(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 1);
}

XrJsonValue *xlsp_handle_td_rename(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    const char *new_name = xjson_get_string(params, "newName");
    if (!textDocument || !position || !new_name)
        return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    return xlsp_analyze_rename(server, doc, pos, new_name);
}

XrJsonValue *xlsp_handle_td_prepare_rename(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position)
        return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    return xlsp_analyze_prepare_rename(doc, pos);
}

XrJsonValue *xlsp_handle_td_formatting(XrLspServer *server,XrJsonValue *params) {
    return xlsp_format_handle(server,params);
}

XrJsonValue *xlsp_handle_td_on_type_formatting(XrLspServer *server,XrJsonValue *params) {
    return xlsp_on_type_handle(server,params);
}

XrJsonValue *xlsp_handle_td_code_lens(XrLspServer *server,XrJsonValue *params) {
    return xlsp_code_lens_handle(server,params);
}

XrJsonValue *xlsp_handle_td_signature_help(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *position = xjson_get_object(params, "position");
    if (!textDocument || !position)
        return xjson_new_null();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_null();

    XrLspPosition pos = {.line = (uint32_t) xjson_get_int(position, "line"),
                         .character = (uint32_t) xjson_get_int(position, "character")};

    return xlsp_analyze_signature_help(doc, pos);
}

_Static_assert(XLSP_TOKEN_FUNCTION==12 && XLSP_TOKEN_PARAMETER==7 && XLSP_TOKEN_MODIFIER==16 && XLSP_TOKEN_COUNT==22, "semantic legend order");
_Static_assert(XLSP_MOD_DECLARATION==1 && XLSP_MOD_DEFINITION==2 && XLSP_MOD_READONLY==4 && XLSP_MOD_MODIFICATION==64, "semantic modifier bits");
XrJsonValue *xlsp_handle_td_semantic_tokens_full(XrLspServer *server,XrJsonValue *params) {return xlsp_semantic_handle(server,params,0);}
XrJsonValue *xlsp_handle_td_semantic_tokens_delta(XrLspServer *server,XrJsonValue *params) {return xlsp_semantic_handle(server,params,1);}
XrJsonValue *xlsp_handle_td_semantic_tokens_range(XrLspServer *server,XrJsonValue *params) {return xlsp_semantic_handle(server,params,2);}

XrJsonValue *xlsp_handle_td_inlay_hint(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *textDocument = xjson_get_object(params, "textDocument");
    XrJsonValue *range_obj = xjson_get_object(params, "range");
    if (!textDocument || !range_obj)
        return xjson_new_array();

    const char *uri = xjson_get_string(textDocument, "uri");
    XrLspDocument *doc = xlsp_document_get(server, uri);
    if (!doc)
        return xjson_new_array();

    XrJsonValue *start = xjson_get_object(range_obj, "start");
    XrJsonValue *end = xjson_get_object(range_obj, "end");

    XrLspRange range = {.start = {.line = (uint32_t) xjson_get_int(start, "line"),
                                  .character = (uint32_t) xjson_get_int(start, "character")},
                        .end = {.line = (uint32_t) xjson_get_int(end, "line"),
                                .character = (uint32_t) xjson_get_int(end, "character")}};

    return xlsp_analyze_inlay_hints(server, doc, range);
}

// Folding range, code action, call/type hierarchy, highlight,
// workspace symbol, selection range, document link handlers
// are now in their own files. See xlsp_folding.c, xlsp_code_action.c,
// xlsp_call_hierarchy.c, xlsp_extra_handlers.c
