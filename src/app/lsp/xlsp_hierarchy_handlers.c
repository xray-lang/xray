/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_hierarchy_handlers.c - Capture real workspace input for Source hierarchies
 */
#include "xlsp_call_hierarchy.h"
#include "xlsp_hierarchy_request.h"

static XrJsonValue *hierarchy_handle(XrLspServer *server, XrJsonValue *params, unsigned mode, bool types) {
    if (!server) return NULL;
    uint64_t revision = server->source_revision;
    uint64_t generation = server->doc_table ? server->doc_table->generation : 0;
    XlspHierarchyRequest *request = NULL;
    XlspSourceRequestResult result = {0};
    XrXirStatus status = xlsp_hierarchy_request_capture(server, params, mode, types, &request);
    if (status == XR_XIR_OK) status = xlsp_hierarchy_request_run(request, NULL, &result);
    if (status == XR_XIR_OK) status = result.status;
    xlsp_hierarchy_request_free(request);
    if (status == XR_XIR_OK && (server->shutdown_received || server->source_revision != revision ||
        (server->doc_table ? server->doc_table->generation : 0) != generation)) status = XR_XIR_UNRESOLVED;
    if (status != XR_XIR_OK) { xjson_free(result.json); result.json = NULL; }
    server->source_failure.status = (unsigned)status;
    server->source_failure.stage = status == XR_XIR_OK ? 0u : types ? 25u : 24u;
    return result.json;
}
XR_FUNC XrJsonValue *xlsp_handle_prepare_call_hierarchy(XrLspServer *server,XrJsonValue *params){return hierarchy_handle(server,params,0,false);}
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_incoming(XrLspServer *server,XrJsonValue *params){return hierarchy_handle(server,params,1,false);}
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_outgoing(XrLspServer *server,XrJsonValue *params){return hierarchy_handle(server,params,2,false);}
XR_FUNC XrJsonValue *xlsp_handle_prepare_type_hierarchy(XrLspServer *server,XrJsonValue *params){return hierarchy_handle(server,params,0,true);}
XR_FUNC XrJsonValue *xlsp_handle_type_hierarchy_supertypes(XrLspServer *server,XrJsonValue *params){return hierarchy_handle(server,params,1,true);}
XR_FUNC XrJsonValue *xlsp_handle_type_hierarchy_subtypes(XrLspServer *server,XrJsonValue *params){return hierarchy_handle(server,params,2,true);}
