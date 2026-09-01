/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir.h - Private AOT realization of a validated XrProgram
 *
 * KEY CONCEPT:
 *   XrBackendIR is a disposable target realization. It copies verified
 *   program structure and adds physical C representation choices, but it
 *   cannot infer or replace language semantics.
 */

#ifndef XR_BACKEND_IR_H
#define XR_BACKEND_IR_H

#include "../../execution/xr_execution.h"

#define XR_BACKEND_IR_SCHEMA_VERSION UINT32_C(1)
#define XR_AOT_TOOLCHAIN_SCHEMA_VERSION UINT32_C(1)
#define XR_NATIVE_ARTIFACT_SCHEMA_VERSION UINT32_C(1)
#define XR_AOT_BACKEND_NAME "xray-c11-aot"
#define XR_AOT_BACKEND_VERSION UINT32_C(1)

typedef XrFingerprint XrBackendId;
typedef XrFingerprint XrOptimizationPolicyId;
typedef XrFingerprint XrToolchainId;
typedef XrFingerprint XrNativeArtifactId;

typedef enum XrBackendOptimizationPolicy {
    XR_BACKEND_OPTIMIZATION_INVALID = 0,
    XR_BACKEND_OPTIMIZATION_NONE = 1,
    XR_BACKEND_OPTIMIZATION_PORTABLE = 2,
} XrBackendOptimizationPolicy;

typedef struct XrBackendOptions {
    uint32_t schema_version;
    uint8_t optimization_policy;
    uint8_t reserved8[3];
    uint32_t max_functions;
    uint32_t max_blocks;
    uint32_t max_instructions;
    uint32_t max_values;
} XrBackendOptions;

typedef enum XrBackendStatus {
    XR_BACKEND_OK = 0,
    XR_BACKEND_INVALID_INPUT,
    XR_BACKEND_INSTANCE_UNAVAILABLE,
    XR_BACKEND_UNSUPPORTED_OPERATION,
    XR_BACKEND_RESOURCE_LIMIT,
    XR_BACKEND_OUT_OF_MEMORY,
    XR_BACKEND_INVARIANT_REJECTED,
    XR_BACKEND_TRANSLATION_REJECTED,
    XR_BACKEND_EMISSION_REJECTED,
    XR_BACKEND_TOOLCHAIN_REJECTED,
    XR_BACKEND_ARTIFACT_REJECTED,
} XrBackendStatus;

typedef struct XrBackendDiagnostic {
    XrBackendStatus status;
    uint16_t operation_id;
    uint16_t reserved16;
    uint32_t function_id;
    uint32_t block_id;
    uint32_t instruction_id;
} XrBackendDiagnostic;

typedef struct XrBackendIR XrBackendIR;

typedef struct XrGeneratedC {
    char *bytes;
    size_t size;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint target_profile_id;
    XrFingerprint source_digest;
} XrGeneratedC;

typedef enum XrAotToolchainProvider {
    XR_AOT_TOOLCHAIN_INVALID = 0,
    XR_AOT_TOOLCHAIN_CLANG = 1,
    XR_AOT_TOOLCHAIN_GCC = 2,
    XR_AOT_TOOLCHAIN_MSVC = 3,
    XR_AOT_TOOLCHAIN_ZIG = 4,
} XrAotToolchainProvider;

typedef struct XrAotToolchainInput {
    uint32_t schema_version;
    uint8_t provider;
    uint8_t reserved8[3];
    const char *provider_version;
    const char *target_triple;
    const char *codegen_options;
    XrFingerprint sysroot_id;
    XrFingerprint runtime_objects_id;
    XrFingerprint target_profile_id;
} XrAotToolchainInput;

typedef struct XrAotToolchainBinding {
    uint32_t schema_version;
    uint8_t provider;
    uint8_t reserved8[3];
    XrFingerprint provider_version_id;
    XrFingerprint target_triple_id;
    XrFingerprint codegen_options_id;
    XrFingerprint sysroot_id;
    XrFingerprint runtime_objects_id;
    XrFingerprint target_profile_id;
    XrToolchainId id;
} XrAotToolchainBinding;

typedef struct XrNativeArtifact {
    uint8_t *bytes;
    size_t size;
    uint32_t schema_version;
    uint32_t reserved32;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrToolchainId toolchain_id;
    XrAotToolchainBinding toolchain_binding;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint target_profile_id;
    XrFingerprint native_digest;
    XrNativeArtifactId id;
} XrNativeArtifact;

XR_FUNC XrBackendOptions xr_backend_default_options(void);
XR_FUNC XrBackendStatus xr_backend_ir_build(XrInstance *instance, const XrBackendOptions *options,
                                            XrBackendIR **ir_out,
                                            XrBackendDiagnostic *diagnostic_out);
XR_FUNC void xr_backend_ir_free(XrBackendIR *ir);
XR_FUNC bool xr_backend_ir_verify(const XrBackendIR *ir, XrBackendDiagnostic *diagnostic_out);
XR_FUNC bool xr_backend_ir_translation_validate(const XrBackendIR *ir,
                                                XrBackendDiagnostic *diagnostic_out);
XR_FUNC XrExecutionId xr_backend_ir_execution_id(const XrBackendIR *ir);
XR_FUNC XrBackendId xr_backend_ir_backend_id(const XrBackendIR *ir);
XR_FUNC XrOptimizationPolicyId xr_backend_ir_optimization_policy_id(const XrBackendIR *ir);
XR_FUNC XrFingerprint xr_backend_ir_lowering_digest(const XrBackendIR *ir);
XR_FUNC size_t xr_backend_ir_instruction_count(const XrBackendIR *ir);
XR_FUNC XrBackendStatus xr_backend_ir_emit_c(const XrBackendIR *ir, bool standalone_main,
                                             XrGeneratedC *generated_out,
                                             XrBackendDiagnostic *diagnostic_out);
XR_FUNC void xr_generated_c_free(XrGeneratedC *generated);

XR_FUNC bool xr_aot_toolchain_binding_build(const XrAotToolchainInput *input,
                                            XrAotToolchainBinding *binding_out);
XR_FUNC bool xr_aot_toolchain_binding_equal(const XrAotToolchainBinding *left,
                                            const XrAotToolchainBinding *right);
XR_FUNC XrBackendStatus xr_native_artifact_seal(const XrGeneratedC *generated,
                                                const XrAotToolchainBinding *toolchain,
                                                const uint8_t *native_bytes, size_t native_size,
                                                XrNativeArtifact *artifact_out);
XR_FUNC bool xr_native_artifact_verify(const XrNativeArtifact *artifact, XrExecutionId execution_id,
                                       XrBackendId backend_id,
                                       XrOptimizationPolicyId optimization_policy_id,
                                       const XrAotToolchainBinding *toolchain);
XR_FUNC void xr_native_artifact_free(XrNativeArtifact *artifact);
XR_FUNC const char *xr_backend_status_name(XrBackendStatus status);

#endif  // XR_BACKEND_IR_H
