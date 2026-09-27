/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_query_internal.h - Source-owner snapshot publication boundary
 *
 * KEY CONCEPT:
 *   Publication copies only semantic facts, under the source owner's budget.
 */
#ifndef XXIR_SOURCE_QUERY_INTERNAL_H
#define XXIR_SOURCE_QUERY_INTERNAL_H
#include "xxir_source_query.h"
XR_FUNC XrXirStatus xr_xir_source_snapshot_copy(const XrXirSourceView *view,
    XrXirBudget *remaining, XrXirSourceSnapshot **output);
#endif // XXIR_SOURCE_QUERY_INTERNAL_H
