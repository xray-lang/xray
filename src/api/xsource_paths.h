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
/* Default precedence after XRAY_STDLIB_PATH: <exe>/../stdlib,
 * <exe>/../lib/xray/stdlib, <cwd>/stdlib, then, only in an uninstalled build
 * configured with XRAY_SOURCE_TREE_STDLIB, the checkout's own stdlib. */
typedef enum XrStdlibOrigin {
    XR_STDLIB_ENVIRONMENT, XR_STDLIB_EXECUTABLE_DEVELOPMENT,
    XR_STDLIB_EXECUTABLE_INSTALLED, XR_STDLIB_WORKING_DIRECTORY, XR_STDLIB_SOURCE_TREE
} XrStdlibOrigin;
typedef struct XrSourcePaths {
    char *entry, *stdlib;
    XrStdlibOrigin stdlib_origin;
} XrSourcePaths;
/* The explicit override or the four default layouts, whichever applies. */
#define XR_SOURCE_STDLIB_CANDIDATE_LIMIT 4u
/* A failed standard-library selection owns the absolute spelling of every
 * rejected candidate, in probe order, so a consumer can say where it looked.
 * The spellings retain the caller ledger like the selected paths do. Success
 * and entry failures own no candidates. Release with
 * xr_source_paths_diagnostic_free(). */
typedef struct XrSourcePathsDiagnostic {
    XrSourceCompileStatus status;
    XrSourcePathStage stage;
    XrOsIoStatus io_status;
    uint32_t candidate_count;
    char *candidates[XR_SOURCE_STDLIB_CANDIDATE_LIMIT];
} XrSourcePathsDiagnostic;

typedef struct XrStdlibPath {
    char *path;
    XrStdlibOrigin origin;
} XrStdlibPath;
/* Select the real standard-library path independently of entry storage. This
 * uses exactly the file entry's environment/executable/cwd precedence. It
 * grants no source or Catalog authority; callers still load the actual
 * descriptor/Catalog and admit their file or owned text entry. No dummy file
 * path is accepted or probed. Failure preserves an initially empty output.
 * A diagnostic that still owns candidates is a bad argument and is untouched. */
XR_FUNC XrSourceCompileStatus xr_source_stdlib_path(XrCompileResources *resources,
    XrStdlibPath *output, XrSourcePathsDiagnostic *diagnostic);
XR_FUNC void xr_source_stdlib_path_free(XrStdlibPath *path);

/* Output must be zero-initialized and changes only on success. Relative input
 * follows the process working directory. A nonempty XRAY_STDLIB_PATH is
 * mandatory when present; otherwise only absent candidates permit trying the
 * next layout. These paths locate inputs; they do not grant module authority.
 * Both strings retain the caller ledger independently of its external owner.
 * A diagnostic that still owns candidates is a bad argument and is untouched. */
XR_FUNC XrSourceCompileStatus xr_source_paths(XrCompileResources *resources,
    const char *entry_spelling, XrSourcePaths *output, XrSourcePathsDiagnostic *diagnostic);
XR_FUNC void xr_source_paths_free(XrSourcePaths *paths);

/* Renders one failed lookup as "source path lookup failed (stage=N
 * status=NAME io=N)". A standard-library failure adds one indented line per
 * rejected candidate and the XRAY_STDLIB_PATH hint. The text is truncated to
 * fit. SIZE must be nonzero. */
XR_FUNC void xr_source_paths_diagnostic_format(const XrSourcePathsDiagnostic *diagnostic,
    char *out, size_t size);
XR_FUNC void xr_source_paths_diagnostic_free(XrSourcePathsDiagnostic *diagnostic);
#endif // XSOURCE_PATHS_H
