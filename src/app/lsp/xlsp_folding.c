/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xlsp_folding.h"
#include "xlsp_hierarchy_request.h"
XR_FUNC XrJsonValue *xlsp_handle_folding_range(XrLspServer *server,XrJsonValue *params) {
    return xlsp_source_scope_query(server,params,0,XLSP_SOURCE_FOLDING);
}
