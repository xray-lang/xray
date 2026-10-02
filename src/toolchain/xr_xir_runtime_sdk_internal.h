/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk_internal.h - Bounded runtime bundle representation
 *
 * KEY CONCEPT:
 *   The closed manifest borrows decoded strings from one owning byte buffer.
 */
#ifndef XR_XIR_RUNTIME_SDK_INTERNAL_H
#define XR_XIR_RUNTIME_SDK_INTERNAL_H
#include "xr_xir_runtime_sdk.h"
#include <stddef.h>
#include <stdint.h>
#define XR_XIR_SDK_MANIFEST_LIMIT (2u * 1024u * 1024u)
#define XR_XIR_SDK_FILE_LIMIT 512u
#define XR_XIR_SDK_PATH_LIMIT 1024u
#define XR_XIR_SDK_FILE_BYTES (UINT64_C(256) * 1024u * 1024u)
#define XR_XIR_SDK_BUNDLE_BYTES (UINT64_C(512) * 1024u * 1024u)
typedef struct XrXirSdkAbiField { uint32_t id, value; } XrXirSdkAbiField;
typedef struct XrXirSdkRecipeFile { const char *path; uint32_t kind; } XrXirSdkRecipeFile;
typedef struct XrXirSdkFile {
    const char *path;
    uint32_t kind;
    uint64_t length;
    uint8_t digest[32];
    void *handle;
    const char *absolute_path;
} XrXirSdkFile;
typedef struct XrXirSdkManifest {
    uint32_t prefix[17];
    const char *target_triple, *abi_recipe, *closure_recipe;
    XrXirSdkFile files[XR_XIR_SDK_FILE_LIMIT];
    uint32_t file_count;
    uint64_t bundle_bytes;
    uint8_t digest[32];
} XrXirSdkManifest;
#endif // XR_XIR_RUNTIME_SDK_INTERNAL_H
