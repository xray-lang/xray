/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_workspace_internal.h - Private immutable workspace owner layout
 *
 * KEY CONCEPT:
 *   Optional query adapters share ownership without adding baseline link edges.
 */
#ifndef XLSP_SOURCE_WORKSPACE_INTERNAL_H
#define XLSP_SOURCE_WORKSPACE_INTERNAL_H
#include "xlsp_source_workspace.h"
#include "../../api/xsource_paths.h"
#include "../../api/xsource_authority.h"
#include "../../xir/xxir_library_catalog.h"
struct XlspSourceWorkspace {
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrStdlibPath stdlib;
    XrXirLibraryCatalog *catalog;
    XrSourceAuthority **authorities;
    XlspSourceSnapshot **snapshots;
    size_t count;
};
#endif // XLSP_SOURCE_WORKSPACE_INTERNAL_H
