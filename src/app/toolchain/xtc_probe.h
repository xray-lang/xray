/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_probe.h - Capability-based provider selection and real compile/link/run probe
 */

#ifndef XTC_PROBE_H
#define XTC_PROBE_H

#include "xtc_discovery.h"
#include "xtc_runtime_manifest.h"

#define XTC_MAX_DIAGNOSTICS 8

typedef enum XrToolchainProfile {
    XR_TOOLCHAIN_PROFILE_HOSTED = 0,
    XR_TOOLCHAIN_PROFILE_FREESTANDING,
} XrToolchainProfile;

typedef enum XrToolchainCapabilityState {
    XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED = 0,
    XR_TOOLCHAIN_CAPABILITY_SKIPPED,
    XR_TOOLCHAIN_CAPABILITY_FAILED,
    XR_TOOLCHAIN_CAPABILITY_OK,
} XrToolchainCapabilityState;

typedef enum XrToolchainCodegenCapability {
    XR_TOOLCHAIN_CODEGEN_FORCE_INLINE = 1u << 0,
    XR_TOOLCHAIN_CODEGEN_PRESERVE_CALL = 1u << 1,
    XR_TOOLCHAIN_CODEGEN_VALUE_OPAQUE = 1u << 2,
    XR_TOOLCHAIN_CODEGEN_COMPILER_FENCE = 1u << 3,
    XR_TOOLCHAIN_CODEGEN_ALL = (1u << 4) - 1u,
} XrToolchainCodegenCapability;

typedef struct XrToolchainDiagnostic {
    XrToolchainReasonCode code;
    char stage[32];
    char message[512];
} XrToolchainDiagnostic;

typedef struct XrToolchainProbeOptions {
    XrToolchainRequest request;
    char cc_storage[1200];
    char zig_storage[1200];
    XrToolchainProfile profile;
    bool no_run;
    bool refresh;
    bool keep_probe;
    /* Native builds and doctor require the complete public code-shape surface.
     * A focused hard contract may request the exact subset it realizes. */
    uint32_t required_codegen_capabilities;
} XrToolchainProbeOptions;

typedef struct XrToolchainProbeResult {
    XrToolchainSelection selection;
    XrRuntimeArtifactSet runtime;
    XrToolchainCapabilityState c_compile;
    XrToolchainCapabilityState sdk_compile;
    XrToolchainCapabilityState runtime_link;
    XrToolchainCapabilityState native_run;
    XrToolchainCapabilityState cross;
    XrToolchainCapabilityState lto;
    XrToolchainCapabilityState force_inline;
    XrToolchainCapabilityState preserve_call;
    XrToolchainCapabilityState value_opaque;
    XrToolchainCapabilityState compiler_fence;
    XrToolchainDiagnostic diagnostics[XTC_MAX_DIAGNOSTICS];
    size_t diagnostic_count;
    char probe_id[40];
    char cache[16];
    uint64_t duration_ms;
} XrToolchainProbeResult;

XR_FUNC bool xtc_profile_parse(const char *text, XrToolchainProfile *out, char *err,
                               size_t err_size);
XR_FUNC const char *xtc_profile_name(XrToolchainProfile profile);
XR_FUNC const char *xtc_capability_state_name(XrToolchainCapabilityState state);
XR_FUNC bool xtc_probe(const XrToolchainProbeOptions *options, XrToolchainProbeResult *out,
                       char *err, size_t err_size);

#endif /* XTC_PROBE_H */
