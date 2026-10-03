/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_local_toolchain.h - Owned discovery of local native build inputs
 */
#ifndef XTC_XIR_LOCAL_TOOLCHAIN_H
#define XTC_XIR_LOCAL_TOOLCHAIN_H
#include "xtc_xir_invocation.h"

typedef struct XtcXirLocalToolchain XtcXirLocalToolchain;
typedef enum XtcXirLocalStatus {
    XTC_XIR_LOCAL_OK, XTC_XIR_LOCAL_INVALID, XTC_XIR_LOCAL_UNRESOLVED,
    XTC_XIR_LOCAL_UNSUPPORTED, XTC_XIR_LOCAL_BUDGET, XTC_XIR_LOCAL_OUT_OF_MEMORY,
    XTC_XIR_LOCAL_IO, XTC_XIR_LOCAL_TIMEOUT, XTC_XIR_LOCAL_CHILD_FAILED
} XtcXirLocalStatus;
typedef enum XtcXirLocalStage {
    XTC_XIR_LOCAL_REQUEST, XTC_XIR_LOCAL_WORKSPACE, XTC_XIR_LOCAL_DISCOVERY,
    XTC_XIR_LOCAL_ENVIRONMENT, XTC_XIR_LOCAL_PATHS, XTC_XIR_LOCAL_SDK
} XtcXirLocalStage;
typedef enum XtcXirLocalDomain {
    XTC_XIR_LOCAL_SELF, XTC_XIR_LOCAL_RESOURCE, XTC_XIR_LOCAL_FILESYSTEM,
    XTC_XIR_LOCAL_PROCESS, XTC_XIR_LOCAL_RUNTIME_SDK
} XtcXirLocalDomain;
typedef struct XtcXirLocalDiagnostic {
    XtcXirLocalStatus status;
    XtcXirLocalStage stage;
    XtcXirLocalDomain domain;
    int code;
    uint32_t os_error;
    int child_exit;
} XtcXirLocalDiagnostic;
typedef struct XtcXirLocalToolchainRequest {
    const char *compiler, *sdk_root, *workspace_parent;
    uint32_t timeout_ms;
    size_t output_limit, manifest_limit;
} XtcXirLocalToolchainRequest;
typedef struct XtcXirLocalToolchainView {
    const char *compiler, *linker, *workspace_parent;
    XrXirMsvcRecipe msvc;
    XrXirInvocationLibrary libraries[5];
    uint32_t library_count;
} XtcXirLocalToolchainView;

/* Windows x64 MSVC only. NULL paths select automatic discovery; explicit paths
 * never fall back. Discovery uses owned child output without modifying the
 * parent environment. SDK admission is real, but these paths do not grant
 * Target or execution authority. All memory and work use the required caller
 * ledger. Output must be NULL and is preserved on failure. Diagnostics are
 * fixed values and retain the first failure without additional allocation.
 * Requests are borrowed only during open; calls require caller exclusion. */
XR_FUNC XtcXirLocalStatus xtc_xir_local_toolchain_open(XrCompileResources *resources,
    const XtcXirLocalToolchainRequest *request, XtcXirLocalToolchain **output,
    XtcXirLocalDiagnostic *diagnostic);
/* Immutable borrows survive destruction of request text and the external
 * ledger reference, and last until free. Queries do not consume work. */
XR_FUNC const XtcXirLocalToolchainView *xtc_xir_local_toolchain_view(const XtcXirLocalToolchain *owner);
XR_FUNC const XrXirRuntimeSdk *xtc_xir_local_toolchain_sdk(const XtcXirLocalToolchain *owner);
XR_FUNC XrCompileResources *xtc_xir_local_toolchain_resources(const XtcXirLocalToolchain *owner);
XR_FUNC void xtc_xir_local_toolchain_free(XtcXirLocalToolchain *owner);
#endif // XTC_XIR_LOCAL_TOOLCHAIN_H
