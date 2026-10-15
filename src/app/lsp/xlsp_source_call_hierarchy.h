/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_call_hierarchy.h - Complete call observations from canonical Source
 */
#ifndef XLSP_SOURCE_CALL_HIERARCHY_H
#define XLSP_SOURCE_CALL_HIERARCHY_H
#include "xlsp_source_bindings.h"
#include "../../base/xjson.h"
typedef struct XlspSourceCallRequest {
    const XrXirCompileContext *context;
    const XlspSourceDocument *documents;
    const bool *known_versions;
    size_t count,entry;
    const XrJsonValue *params;
    uint64_t revision;
    unsigned mode; /* 0 prepare, 1 incoming, 2 outgoing. */
} XlspSourceCallRequest;
/* Inputs are synchronous borrows of a complete captured corpus. Source owns
 * binding and caller identity. Missing indirect targets or relevant ranges
 * reject the complete response. Opaque item data authenticates the current
 * revision, source fingerprint and known editor version without persisting
 * local declaration IDs. Output owns all JSON after every producer dies. */
XR_FUNC XrXirStatus xlsp_source_call_hierarchy_json(const XlspSourceCallRequest *,XrJsonValue **);
#endif
