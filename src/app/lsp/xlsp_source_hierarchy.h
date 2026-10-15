/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_hierarchy.h - One captured corpus for canonical hierarchy requests
 */
#ifndef XLSP_SOURCE_HIERARCHY_H
#define XLSP_SOURCE_HIERARCHY_H
#include "xlsp_source_bindings.h"
#include "../../base/xjson.h"
typedef struct XlspSourceHierarchyRequest {
    const XrXirCompileContext *context;
    const XlspSourceDocument *documents;
    const bool *known_versions;
    size_t count,entry;
    const XrJsonValue *params;
    uint64_t revision;
    /* 0 prepare; 1 incoming calls or supertypes; 2 outgoing calls or subtypes. */
    unsigned mode;
} XlspSourceHierarchyRequest;
#endif // XLSP_SOURCE_HIERARCHY_H
