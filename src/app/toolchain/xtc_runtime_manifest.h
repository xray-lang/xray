/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_runtime_manifest.h - Exact target/ABI/provider runtime artifact resolver
 */

#ifndef XTC_RUNTIME_MANIFEST_H
#define XTC_RUNTIME_MANIFEST_H

#include "xtc_model.h"

#define XTC_RUNTIME_MAX_ARTIFACTS 8
#define XTC_RUNTIME_MAX_SYSTEM_LIBS 16

typedef struct XrRuntimeArtifact {
    char id[256];
    char kind[64];
    char path[1200];
    char sha256[65];
} XrRuntimeArtifact;

typedef struct XrRuntimeArtifactSet {
    int schema;
    int sdk_abi;
    char target[128];
    char object_format[32];
    char manifest_path[1200];
    char sdk_digest[72];
    char public_include[1200];
    char private_aot_include[1200];
    XrRuntimeArtifact artifacts[XTC_RUNTIME_MAX_ARTIFACTS];
    size_t artifact_count;
    char system_libraries[XTC_RUNTIME_MAX_SYSTEM_LIBS][64];
    size_t system_library_count;
} XrRuntimeArtifactSet;

XR_FUNC bool xtc_runtime_manifest_load(const XrToolchainTarget *target,
                                       XrToolchainProviderId provider, const char *program_hint,
                                       XrRuntimeArtifactSet *out, XrToolchainReasonCode *reason,
                                       char *err, size_t err_size);
XR_FUNC const XrRuntimeArtifact *xtc_runtime_artifact_find(const XrRuntimeArtifactSet *set,
                                                           const char *id_prefix);

#endif /* XTC_RUNTIME_MANIFEST_H */
