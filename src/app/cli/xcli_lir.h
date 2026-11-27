/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli_lir.h - Shared command compilation and ordinary native publication
 */
#ifndef XCLI_LIR_H
#define XCLI_LIR_H
#include "app/toolchain/xtc_xir_workspace.h"
#include "api/xsource_compile.h"
#include "lir/xlir_test_plan.h"

/* Both outputs must be empty. Success transfers independent owners. */
XR_FUNC int xr_cli_lir_compile(const char *input, XrCompileResources **resources,
    XrLirLowered *output);
typedef struct XrCliTestCompileFailure {
    XrCompileResources *resources;
    XrSourceCompileStatus status;
    XrSourceCompileStage stage;
    char *text;
} XrCliTestCompileFailure;
/* A requested failure owns its rendered text; release it with xr_compile_resources_free. */
XR_FUNC int xr_cli_lir_compile_test_plan(const char *input, const char *filter, bool quiet,
    XrLirTestPlan **output, XrCliTestCompileFailure *cause);
XR_FUNC int xr_cli_lir_publish(XrCompileResources *resources, const char *path,
    const void *bytes, size_t length, bool executable);
typedef struct XrCliNativeOptions {
    const char *compiler, *provider;
    bool verbose;
} XrCliNativeOptions;
/* Returns false on a cleaned workspace with a hard error and reports the first
 * diagnosis. Exits internally if the bounded close still owns the workspace. */
XR_FUNC bool xr_cli_lir_workspace_close(XtcXirWorkspace **workspace, const char *subject);
XR_FUNC int xr_cli_lir_native(XrCompileResources *resources, const XrCliNativeOptions *options,
    const void *source, size_t length, const char *output);
#endif // XCLI_LIR_H
