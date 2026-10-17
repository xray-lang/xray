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
#include "xlsp_hierarchy_request.h"
#include "xlsp_server.h"
#include "../../base/xjson.h"
#include "xlsp_analysis.h"
#include "xlsp_format.h"
#include "xlsp_on_type.h"
#include "xlsp_document_notification.h"
#include "xlsp_workspace.h"
#include "xlsp_semantic_tokens.h"
#include "xlsp_inlay_hints.h"
#include "xlsp_utils.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>

void xlsp_handle_td_did_open(XrLspServer *server, XrJsonValue *params) {
    if (!server) return;
    XlspDocumentNotification input = {0};
    if (xlsp_document_notification_decode(params, XLSP_DOCUMENT_NOTIFICATION_OPEN, &input) != XR_XIR_OK) {
        lsp_log("didOpen rejected: invalid document notification"); return;
    }
    XlspSourceDocument document = {input.uri->as.string, input.text->as.string,
        input.uri->string_len, input.text->string_len, input.version};
    XrLspDocument *doc = xlsp_document_open(server, &document);
    if (!doc) { lsp_log("didOpen failed; document table unchanged"); return; }
    if (server->config.diagnostics_enabled) xlsp_publish_diagnostics(server, doc);
}

void xlsp_handle_td_did_change(XrLspServer *server, XrJsonValue *params) {
    if (!server) return;
    XlspDocumentNotification input = {0};
    if (xlsp_document_notification_decode(params, XLSP_DOCUMENT_NOTIFICATION_CHANGE, &input) != XR_XIR_OK) {
        lsp_log("didChange rejected: invalid document notification"); return;
    }
    XrLspDocument *doc = xlsp_document_get(server, input.uri->as.string);
    if (!doc) return;
    XrXirStatus status = xlsp_document_apply_changes(doc, input.changes, input.version);
    if (status != XR_XIR_OK) {
        lsp_log("didChange rejected: status %u, previous version retained", (unsigned)status); return;
    }
    xlsp_schedule_diagnostics(server, doc);
}

void xlsp_handle_td_did_close(XrLspServer *server, XrJsonValue *params) {
    if (!server) return;
    XlspDocumentNotification input = {0};
    if (xlsp_document_notification_decode(params, XLSP_DOCUMENT_NOTIFICATION_CLOSE, &input) != XR_XIR_OK) {
        lsp_log("didClose rejected: invalid document notification"); return;
    }
    xlsp_document_close(server, input.uri->as.string);
    xlsp_workspace_index_file(server, input.uri->as.string, NULL);
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

/* Completion producers include display fields in each item. Resolve copies
 * those fields without borrowing mutable editor or analyzer state. */
XR_FUNC XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    if (!server) return NULL;
    server->source_failure.stage = 23;
    server->source_failure.status = XR_XIR_BAD_STRUCTURE;
    if (!params || params->type != XR_JSON_OBJECT || params->as.object.count < 0 ||
        params->as.object.capacity < params->as.object.count ||
        (params->as.object.count && !params->as.object.members)) return NULL;
    const XrJsonValue *label = NULL;
    for (int i = 0; i < params->as.object.count; ++i) {
        const XrJsonMember *member = &params->as.object.members[i];
        if (!member->key || !member->value) return NULL;
        if (member->key_len != 5 || memcmp(member->key, "label", 5)) continue;
        if (label) return NULL;
        label = member->value;
    }
    if (!label || label->type != XR_JSON_STRING) return NULL;
    XrJsonValue *result = xjson_clone(params);
    server->source_failure.status = result ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY;
    if (result) server->source_failure.stage = 0;
    return result;
}

XrJsonValue *xlsp_handle_td_hover(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 3);
}

XR_FUNC XrJsonValue *xlsp_handle_td_document_symbol(XrLspServer *server,XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_DOCUMENT_SYMBOLS);
}

XrJsonValue *xlsp_handle_td_definition(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 0);
}

XrJsonValue *xlsp_handle_td_references(XrLspServer *server, XrJsonValue *params) {
    return xlsp_navigation_handle(server, params, 1);
}

XrJsonValue *xlsp_handle_td_rename(XrLspServer *server, XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_RENAME);
}

XrJsonValue *xlsp_handle_td_prepare_rename(XrLspServer *server, XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_PREPARE_RENAME);
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
