/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_call_hierarchy.h - Call hierarchy and type hierarchy support
 */

#ifndef XLSP_CALL_HIERARCHY_H
#define XLSP_CALL_HIERARCHY_H

#include "xlsp_server.h"
#include "../../base/xjson.h"

// Call hierarchy
XR_FUNC XrJsonValue *xlsp_handle_prepare_call_hierarchy(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_incoming(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_call_hierarchy_outgoing(XrLspServer *server, XrJsonValue *params);

// Type hierarchy
XR_FUNC XrJsonValue *xlsp_handle_prepare_type_hierarchy(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_type_hierarchy_supertypes(XrLspServer *server, XrJsonValue *params);
XR_FUNC XrJsonValue *xlsp_handle_type_hierarchy_subtypes(XrLspServer *server, XrJsonValue *params);

// Implementation (delegates to definition)
XR_FUNC XrJsonValue *xlsp_handle_implementation(XrLspServer *server, XrJsonValue *params);

#endif // XLSP_CALL_HIERARCHY_H
