/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_dependencies.h - Owned observation of the admitted source dependency graph
 */
#ifndef XXIR_SOURCE_DEPENDENCIES_H
#define XXIR_SOURCE_DEPENDENCIES_H
#include "xxir_source_query.h"
typedef enum XrXirSourceDependencyKind {
    XR_XIR_DEPENDENCY_STDLIB, XR_XIR_DEPENDENCY_FILE,
    XR_XIR_DEPENDENCY_PACKAGE, XR_XIR_DEPENDENCY_MEMORY
} XrXirSourceDependencyKind;
typedef struct XrXirSourceDependency {
    uint32_t module;
    XrXirSourceDependencyKind kind;
    const char *path;
    const uint32_t *imports;
    uint32_t import_count;
} XrXirSourceDependency;
/* Entries are in the actual graph's dependency-first topological order.
 * module/imports use SourceView module ordinals; entry indexes this entry array.
 * Strings and arrays belong to the snapshot. These facts confer no authority. */
typedef struct XrXirSourceDependencies {
    const XrXirSourceDependency *entries;
    uint32_t count, entry;
    const char *entry_path;
} XrXirSourceDependencies;
XR_FUNC const XrXirSourceDependencies *xr_xir_compile_source_snapshot_dependencies(
    const XrXirSourceSnapshot *snapshot);
#endif // XXIR_SOURCE_DEPENDENCIES_H
