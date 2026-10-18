/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_type_hierarchy.h - Project exact Source type relations into protocol values
 */
#ifndef XLSP_SOURCE_TYPE_HIERARCHY_H
#define XLSP_SOURCE_TYPE_HIERARCHY_H
#include "xlsp_source_hierarchy.h"
/* Checked tables alone select direct interface parents and explicit nominal
 * implementations. Unknown identities or bounds reject the whole response.
 * Output owns all JSON after corpus, Source and editor producers are freed. */
XR_FUNC XrXirStatus xlsp_source_type_hierarchy_json(const XlspSourceHierarchyRequest *,XrJsonValue **);
#endif // XLSP_SOURCE_TYPE_HIERARCHY_H
