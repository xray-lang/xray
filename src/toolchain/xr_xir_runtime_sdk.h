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

typedef enum XrXirRuntimeSdkStatus {
    XR_XIR_SDK_OK = 0, XR_XIR_SDK_INVALID = 1, XR_XIR_SDK_UNRESOLVED = 2,
    XR_XIR_SDK_BUDGET = 3, XR_XIR_SDK_OUT_OF_MEMORY = 4,
    XR_XIR_SDK_IO = 5, XR_XIR_SDK_UNSUPPORTED = 6
} XrXirRuntimeSdkStatus;
typedef struct XrXirRuntimeSdk XrXirRuntimeSdk;
typedef struct XrXirRuntimeSdkRequest {
    const char *root;
    const void *manifest;
    size_t manifest_length;
    uint64_t metadata_limit, work_limit;
} XrXirRuntimeSdkRequest;
typedef struct XrXirRuntimeSdkFacts {
    uint32_t schema, wire, semantic, value_abi, call_abi, program_abi;
    uint32_t architecture, object_format, hosted, c_dialect, crt, sanitizers;
    uint32_t allocator, assertions, build_provider, abi_recipe_version, closure_recipe_version;
    uint32_t file_count;
    uint64_t file_bytes, metadata_bytes, work_used;
    uint8_t identity[32];
} XrXirRuntimeSdkFacts;
/* Requests are borrowed synchronously. Failure preserves every output byte.
 * The consumer must keep the SDK alive until compiler and linker have exited. */
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_load(const XrXirRuntimeSdkRequest *request,
    XrXirRuntimeSdk **output);
XR_FUNC const XrXirRuntimeSdkFacts *xr_xir_runtime_sdk_facts(const XrXirRuntimeSdk *sdk);
XR_FUNC const char *xr_xir_runtime_sdk_root(const XrXirRuntimeSdk *sdk);
XR_FUNC const char *xr_xir_runtime_sdk_file(const XrXirRuntimeSdk *sdk, const char *relative_path);
XR_FUNC void xr_xir_runtime_sdk_free(XrXirRuntimeSdk *sdk);
#endif // XR_XIR_RUNTIME_SDK_H
