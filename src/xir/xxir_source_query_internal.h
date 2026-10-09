/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_query_internal.h - Source-owner snapshot publication boundary
 *
 * KEY CONCEPT:
 *   Publication copies only semantic facts, using the source owner's resource context.
 */
#ifndef XXIR_SOURCE_QUERY_INTERNAL_H
#define XXIR_SOURCE_QUERY_INTERNAL_H
#include "xxir_source_query.h"
#include "xxir_source_dependencies.h"
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_copy_v2(const XrXirCompileContext *context,
    const XrXirSourceView *view, const XrXirConstruction *construction, XrXirSourceSnapshot **output);
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_syntax_copy(XrXirSourceSnapshot *, const XrXirSourceSyntaxView *);
/* Internal pre-publication attachment only. Failure preserves existing facts;
 * it never authorizes execution or mutates the checked construction owner. */
XR_FUNC XrXirStatus xr_xir_compile_source_snapshot_dependencies_copy(
    XrXirSourceSnapshot *snapshot, const XrXirSourceDependencies *dependencies);
#endif // XXIR_SOURCE_QUERY_INTERNAL_H
