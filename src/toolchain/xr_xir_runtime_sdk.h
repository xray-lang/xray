/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk.h - Owned admission of one same-source runtime bundle
 *
 * KEY CONCEPT:
 *   Immutable metadata and locked files keep validation and consumption equal.
 */
#ifndef XR_XIR_RUNTIME_SDK_H
#define XR_XIR_RUNTIME_SDK_H
#include "../base/xdefs.h"
#include "../base/xcompile_resources.h"

typedef enum XrXirRuntimeSdkStatus {
    XR_XIR_SDK_OK = 0, XR_XIR_SDK_INVALID = 1, XR_XIR_SDK_UNRESOLVED = 2,
    XR_XIR_SDK_BUDGET = 3, XR_XIR_SDK_OUT_OF_MEMORY = 4,
    XR_XIR_SDK_IO = 5, XR_XIR_SDK_UNSUPPORTED = 6,
    XR_XIR_SDK_MEMORY_LIMIT = 7, XR_XIR_SDK_OVERFLOW = 8
} XrXirRuntimeSdkStatus;
typedef struct XrXirRuntimeSdk XrXirRuntimeSdk;
typedef struct XrXirRuntimeSdkRequest {
    const char *root;
    const void *manifest;
    size_t manifest_length;
    XrCompileResources *resources;
} XrXirRuntimeSdkRequest;
/* POSIX bundle facts for this host. macOS and Linux publish their native
 * object format (Mach-O 3, ELF 2) for x86_64 (1) or arm64 (2), linked against
 * the system C library (crt 3). XR_XIR_SDK_POSIX_HOST is defined only where a
 * native executable can be built; Windows keeps its fixed x86_64 MSVC facts. */
#if (defined(XR_OS_MACOS) || defined(XR_OS_LINUX)) && (defined(XR_ARCH_X86_64) || defined(XR_ARCH_ARM64))
#define XR_XIR_SDK_POSIX_HOST 1
#endif
#if defined(XR_XIR_SDK_POSIX_HOST) && defined(XR_ARCH_X86_64)
#define XR_XIR_SDK_POSIX_ARCHITECTURE 1u
#define XR_XIR_SDK_POSIX_CPU "x86_64"
#define XR_XIR_SDK_POSIX_ARCH_NAME "x86_64"
#elif defined(XR_XIR_SDK_POSIX_HOST)
#define XR_XIR_SDK_POSIX_ARCHITECTURE 2u
#define XR_XIR_SDK_POSIX_CPU "aarch64"
#define XR_XIR_SDK_POSIX_ARCH_NAME "arm64"
#endif
#if defined(XR_XIR_SDK_POSIX_HOST) && defined(XR_OS_MACOS)
#define XR_XIR_SDK_POSIX_OBJECT_FORMAT 3u
#define XR_XIR_SDK_POSIX_SYSTEM "apple-darwin"
#define XR_XIR_SDK_POSIX_OS_NAME "darwin"
#elif defined(XR_XIR_SDK_POSIX_HOST)
#define XR_XIR_SDK_POSIX_OBJECT_FORMAT 2u
#define XR_XIR_SDK_POSIX_SYSTEM "linux-gnu"
#define XR_XIR_SDK_POSIX_OS_NAME "linux"
#endif
#define XR_XIR_SDK_POSIX_CRT 3u
#define XR_XIR_SDK_POSIX_TRIPLE XR_XIR_SDK_POSIX_CPU "-" XR_XIR_SDK_POSIX_SYSTEM
#define XR_XIR_SDK_POSIX_CLOSURE_RECIPE \
    "xray:xir-runtime-recipe:" XR_XIR_SDK_POSIX_OS_NAME "-" XR_XIR_SDK_POSIX_ARCH_NAME "-hosted:v1"
typedef struct XrXirRuntimeSdkFacts {
    uint32_t schema, wire, semantic, value_abi, call_abi, program_abi;
    uint32_t architecture, object_format, hosted, c_dialect, crt, sanitizers;
    uint32_t allocator, assertions, build_provider, abi_recipe_version, closure_recipe_version;
    uint32_t file_count;
    uint64_t file_bytes, metadata_bytes;
    uint8_t identity[32];
} XrXirRuntimeSdkFacts;
/* Requests are borrowed synchronously; resources are mandatory and shared with
 * other compiler stages. Failure preserves output, which must initially be NULL.
 * Blocks keep resources alive after the producer releases its external reference.
 * The consumer must keep the SDK alive until compiler and linker have exited.
 * metadata_bytes reports this SDK's resident requested allocation capacity:
 * its body plus SdkMemory headers/payloads, excluding shared-owner blocks and
 * the memory allocator's own headers. It is neither a quota nor turnover. */
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_load(const XrXirRuntimeSdkRequest *request,
    XrXirRuntimeSdk **output);
XR_FUNC const XrXirRuntimeSdkFacts *xr_xir_runtime_sdk_facts(const XrXirRuntimeSdk *sdk);
/* Borrow the owner's original ledger; NULL is returned for a NULL owner.
 * This query does not allocate or scan. */
XR_FUNC XrCompileResources *xr_xir_runtime_sdk_resources(const XrXirRuntimeSdk *sdk);
XR_FUNC const char *xr_xir_runtime_sdk_root(const XrXirRuntimeSdk *sdk);
/* Lookup compares immutable admitted metadata. Success borrows a path; failure
 * preserves output, which must initially be NULL. Unknown paths are UNRESOLVED. */
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_file(const XrXirRuntimeSdk *sdk,
    const char *relative_path, const char **output);
XR_FUNC void xr_xir_runtime_sdk_free(XrXirRuntimeSdk *sdk);
#endif // XR_XIR_RUNTIME_SDK_H
