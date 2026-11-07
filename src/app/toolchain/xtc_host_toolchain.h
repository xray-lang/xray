/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_host_toolchain.h - Owned host compiler paths and platform inputs
 */
#ifndef XTC_HOST_TOOLCHAIN_H
#define XTC_HOST_TOOLCHAIN_H
#include "xtc_process.h"

#if (defined(XR_OS_MACOS) || defined(XR_OS_LINUX)) && \
    (defined(XR_ARCH_X86_64) || defined(XR_ARCH_ARM64))
#define XTC_HOST_POSIX 1
#if defined(XR_ARCH_X86_64)
#define XTC_HOST_ARCH "x86_64"
#else
#define XTC_HOST_ARCH "arm64"
#endif
#endif

typedef struct XtcHostToolchain XtcHostToolchain;
typedef enum XtcHostStatus {
    XTC_HOST_OK, XTC_HOST_INVALID, XTC_HOST_UNRESOLVED,
    XTC_HOST_UNSUPPORTED, XTC_HOST_BUDGET, XTC_HOST_OUT_OF_MEMORY,
    XTC_HOST_IO, XTC_HOST_TIMEOUT, XTC_HOST_CHILD_FAILED,
    XTC_HOST_MEMORY_LIMIT, XTC_HOST_OVERFLOW
} XtcHostStatus;

typedef enum XtcHostStage {
    XTC_HOST_REQUEST, XTC_HOST_WORKSPACE, XTC_HOST_DISCOVERY,
    XTC_HOST_ENVIRONMENT, XTC_HOST_PATHS
} XtcHostStage;
typedef enum XtcHostDomain {
    XTC_HOST_SELF, XTC_HOST_RESOURCE, XTC_HOST_FILESYSTEM, XTC_HOST_PROCESS
} XtcHostDomain;
typedef enum XtcHostCrt {
    XTC_HOST_CRT_STATIC, XTC_HOST_CRT_STATIC_DEBUG,
    XTC_HOST_CRT_DYNAMIC, XTC_HOST_CRT_DYNAMIC_DEBUG
} XtcHostCrt;
typedef struct XtcHostDiagnostic {
    XtcHostStatus status;
    XtcHostStage stage;
    XtcHostDomain domain;
    int code;
    uint32_t os_error;
    int child_exit;
} XtcHostDiagnostic;
typedef struct XtcHostToolchainRequest {
    const char *compiler, *workspace_parent;
    uint32_t timeout_ms;
    size_t output_limit;
    XtcHostCrt crt;
} XtcHostToolchainRequest;
typedef struct XtcHostMsvc {
    const char *vc_include, *ucrt_include, *shared_include, *um_include, *system_root;
    const char *vc_library, *ucrt_library, *um_library;
    bool clang;
} XtcHostMsvc;
typedef struct XtcHostPosix {
    const char *sysroot, *arch, *path;
} XtcHostPosix;
typedef struct XtcHostToolchainView {
    const char *compiler, *linker, *workspace_parent;
    XtcHostMsvc msvc;
    const char *libraries[8];
    uint32_t library_count;
    XtcHostPosix posix;
} XtcHostToolchainView;

/* Request strings are borrowed synchronously. Success owns all paths and
 * selected platform environment facts on the caller's memory owner. A supplied
 * compiler is mandatory when present. Windows discovers the selected MSVC
 * installation without changing the parent environment; clang-cl may use its
 * platform inputs. POSIX resolves one compiler and the active Darwin SDK.
 * Output must be NULL and is preserved on failure. */
XR_FUNC XtcHostStatus xtc_host_toolchain_open(XrCompileResources *resources,
    const XtcHostToolchainRequest *request, XtcHostToolchain **output,
    XtcHostDiagnostic *diagnostic);
XR_FUNC const XtcHostToolchainView *xtc_host_toolchain_view(const XtcHostToolchain *owner);
XR_FUNC void xtc_host_toolchain_free(XtcHostToolchain *owner);
#endif // XTC_HOST_TOOLCHAIN_H
