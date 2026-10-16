/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_links.h"
#include "xlsp_hierarchy_request.h"
#include <string.h>
XR_FUNC XrJsonValue *xlsp_links_handle(XrLspServer *server, XrJsonValue *params) {
    if (!server) return NULL;
    server->source_failure.stage = 15; server->source_failure.status = XR_XIR_BAD_STRUCTURE;
    XrJsonValue *document = xjson_get_object(params, "textDocument"), *uri = xjson_get(document, "uri");
    if (!uri || uri->type != XR_JSON_STRING || !uri->as.string || memchr(uri->as.string, 0, uri->string_len))
        return NULL;
    XrLspDocument *selected = xlsp_document_store_get(server->doc_table, uri->as.string);
    if (!selected || selected->server != server) return NULL;
    return xlsp_source_scope_query(server, params, 0, XLSP_SOURCE_DOCUMENT_LINKS);
}
