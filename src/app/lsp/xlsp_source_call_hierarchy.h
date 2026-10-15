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
#include "xlsp_source_hierarchy.h"
/* Inputs are synchronous borrows of a complete captured corpus. Source owns
 * binding and caller identity. Missing indirect targets or relevant ranges
 * reject the complete response. Opaque item data authenticates the current
 * revision, source fingerprint and known editor version without persisting
 * local declaration IDs. Output owns all JSON after every producer dies. */
XR_FUNC XrXirStatus xlsp_source_call_hierarchy_json(const XlspSourceHierarchyRequest *,XrJsonValue **);
#endif
