/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsource_paths.h - Owned source and standard-library path selection
 */
#ifndef XSOURCE_PATHS_H
#define XSOURCE_PATHS_H
#include "xsource_compile.h"
#include "../base/xio_policy.h"

typedef enum XrSourcePathStage {
    XR_SOURCE_PATH_INPUT, XR_SOURCE_PATH_ENTRY, XR_SOURCE_PATH_ENVIRONMENT,
    XR_SOURCE_PATH_EXECUTABLE, XR_SOURCE_PATH_STDLIB
} XrSourcePathStage;
typedef enum XrStdlibOrigin {
    XR_STDLIB_ENVIRONMENT, XR_STDLIB_EXECUTABLE_DEVELOPMENT,
    XR_STDLIB_EXECUTABLE_INSTALLED, XR_STDLIB_WORKING_DIRECTORY
} XrStdlibOrigin;
typedef struct XrSourcePaths {
    char *entry, *stdlib;
    XrStdlibOrigin stdlib_origin;
} XrSourcePaths;
typedef struct XrSourcePathsDiagnostic {
    XrSourceCompileStatus status;
    XrSourcePathStage stage;
    XrOsIoStatus io_status;
} XrSourcePathsDiagnostic;

typedef struct XrStdlibPath {
    char *path;
    XrStdlibOrigin origin;
} XrStdlibPath;
/* Select the real standard-library path independently of entry storage. This
 * uses exactly the file entry's environment/executable/cwd precedence. It
 * grants no source or Catalog authority; callers still load the actual
 * descriptor/Catalog and admit their file or owned text entry. No dummy file
 * path is accepted or probed. Failure preserves an initially empty output. */
XR_FUNC XrSourceCompileStatus xr_source_stdlib_path(XrCompileResources *resources,
    XrStdlibPath *output, XrSourcePathsDiagnostic *diagnostic);
XR_FUNC void xr_source_stdlib_path_free(XrStdlibPath *path);

/* Output must be zero-initialized and changes only on success. Relative input
 * follows the process working directory. A nonempty XRAY_STDLIB_PATH is
 * mandatory when present; otherwise only absent candidates permit trying the
 * next layout. These paths locate inputs; they do not grant module authority.
 * Both strings retain the caller ledger independently of its external owner. */
XR_FUNC XrSourceCompileStatus xr_source_paths(XrCompileResources *resources,
    const char *entry_spelling, XrSourcePaths *output, XrSourcePathsDiagnostic *diagnostic);
XR_FUNC void xr_source_paths_free(XrSourcePaths *paths);
#endif // XSOURCE_PATHS_H
