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

XR_FUNC XrJsonValue *xlsp_handle_prepare_call_hierarchy(XrLspServer *server,XrJsonValue *params){return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_CALL_HIERARCHY);}
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_incoming(XrLspServer *server,XrJsonValue *params){return xlsp_source_scope_query(server,params,1,XLSP_SOURCE_CALL_HIERARCHY);}
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_outgoing(XrLspServer *server,XrJsonValue *params){return xlsp_source_scope_query(server,params,2,XLSP_SOURCE_CALL_HIERARCHY);}
XR_FUNC XrJsonValue *xlsp_handle_prepare_type_hierarchy(XrLspServer *server,XrJsonValue *params){return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_TYPE_HIERARCHY);}
XR_FUNC XrJsonValue *xlsp_handle_type_hierarchy_supertypes(XrLspServer *server,XrJsonValue *params){return xlsp_source_scope_query(server,params,1,XLSP_SOURCE_TYPE_HIERARCHY);}
XR_FUNC XrJsonValue *xlsp_handle_type_hierarchy_subtypes(XrLspServer *server,XrJsonValue *params){return xlsp_source_scope_query(server,params,2,XLSP_SOURCE_TYPE_HIERARCHY);}
