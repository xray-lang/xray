/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_source_paths.h - Owned source and standard-library path selection
 */
#ifndef XCLI_SOURCE_PATHS_H
#define XCLI_SOURCE_PATHS_H
#include "xcli_canonical_source.h"
#include "../../base/xio_policy.h"

typedef enum XrCliSourcePathStage {
    XR_CLI_SOURCE_PATH_INPUT, XR_CLI_SOURCE_PATH_ENTRY, XR_CLI_SOURCE_PATH_ENVIRONMENT,
    XR_CLI_SOURCE_PATH_EXECUTABLE, XR_CLI_SOURCE_PATH_STDLIB
} XrCliSourcePathStage;
typedef enum XrCliStdlibOrigin {
    XR_CLI_STDLIB_ENVIRONMENT, XR_CLI_STDLIB_EXECUTABLE_DEVELOPMENT,
    XR_CLI_STDLIB_EXECUTABLE_INSTALLED, XR_CLI_STDLIB_WORKING_DIRECTORY
} XrCliStdlibOrigin;
typedef struct XrCliSourcePaths {
    char *entry, *stdlib;
    XrCliStdlibOrigin stdlib_origin;
} XrCliSourcePaths;
typedef struct XrCliSourcePathsDiagnostic {
    XrCliCompileSourceStatus status;
    XrCliSourcePathStage stage;
    XrOsIoStatus io_status;
} XrCliSourcePathsDiagnostic;

/* Output must be zero-initialized and changes only on success. Relative input
 * follows the process working directory. A nonempty XRAY_STDLIB_PATH is
 * mandatory when present; otherwise only absent candidates permit trying the
 * next layout. These paths locate inputs; they do not grant module authority.
 * Both strings retain the caller ledger independently of its external owner. */
XR_FUNC XrCliCompileSourceStatus xr_cli_compile_source_paths(XrCompileResources *resources,
    const char *entry_spelling, XrCliSourcePaths *output, XrCliSourcePathsDiagnostic *diagnostic);
XR_FUNC void xr_cli_compile_source_paths_free(XrCliSourcePaths *paths);
#endif // XCLI_SOURCE_PATHS_H
