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
    XR_XIR_SDK_IO = 5, XR_XIR_SDK_UNSUPPORTED = 6
} XrXirRuntimeSdkStatus;
typedef struct XrXirRuntimeSdk XrXirRuntimeSdk;
typedef struct XrXirRuntimeSdkRequest {
    const char *root;
    const void *manifest;
    size_t manifest_length;
    XrCompileResources *resources;
} XrXirRuntimeSdkRequest;
typedef struct XrXirRuntimeSdkFacts {
    uint32_t schema, wire, semantic, value_abi, call_abi, program_abi;
    uint32_t architecture, object_format, hosted, c_dialect, crt, sanitizers;
    uint32_t allocator, assertions, build_provider, abi_recipe_version, closure_recipe_version;
    uint32_t file_count;
    uint64_t file_bytes, metadata_bytes, work_used;
    uint8_t identity[32];
} XrXirRuntimeSdkFacts;
/* Requests are borrowed synchronously; resources are mandatory and shared with
 * other compiler stages. Failure preserves output, which must initially be NULL.
 * Blocks keep resources alive after the producer releases its external reference.
 * The consumer must keep the SDK alive until compiler and linker have exited.
 * Facts report this load's cumulative allocation/work deltas, never limits. */
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_load(const XrXirRuntimeSdkRequest *request,
    XrXirRuntimeSdk **output);
XR_FUNC const XrXirRuntimeSdkFacts *xr_xir_runtime_sdk_facts(const XrXirRuntimeSdk *sdk);
XR_FUNC const char *xr_xir_runtime_sdk_root(const XrXirRuntimeSdk *sdk);
/* Lookup charges comparisons to the same ledger. Success borrows a path; failure
 * preserves output, which must initially be NULL. Unknown paths are UNRESOLVED. */
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_file(const XrXirRuntimeSdk *sdk,
    const char *relative_path, const char **output);
XR_FUNC void xr_xir_runtime_sdk_free(XrXirRuntimeSdk *sdk);
#endif // XR_XIR_RUNTIME_SDK_H
