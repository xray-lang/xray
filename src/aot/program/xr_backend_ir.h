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
 *   XrBackendIR retains one immutable validated program and its exact target
 *   profile. Emission reads that program directly and selects physical C
 *   representations without copying or rewriting the logical graph.
 */

#ifndef XR_BACKEND_IR_H
#define XR_BACKEND_IR_H

#include "../../execution/xr_native_descriptor.h"

#define XR_BACKEND_IR_SCHEMA_VERSION UINT32_C(3)
#define XR_AOT_BACKEND_NAME "xray-c11-aot"
#define XR_AOT_BACKEND_VERSION UINT32_C(19)

typedef XrFingerprint XrBackendId;
typedef XrFingerprint XrOptimizationPolicyId;

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
    XR_BACKEND_UNSUPPORTED_OPERATION,
    XR_BACKEND_RESOURCE_LIMIT,
    XR_BACKEND_OUT_OF_MEMORY,
    XR_BACKEND_INVARIANT_REJECTED,
    XR_BACKEND_BINDING_REJECTED,
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

/* Borrowed only for one emission; function IDs belong to the retained Program. */
typedef struct XrBackendCExport {
    uint32_t function_id;
    const char *symbol;
    uint8_t hidden;
    uint8_t header;
    uint8_t reserved8[2];
} XrBackendCExport;

typedef struct XrGeneratedC {
    char *bytes;
    size_t size;
    char *header_bytes;
    size_t header_size;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint target_profile_id;
    XrFingerprint source_digest;
} XrGeneratedC;

XR_FUNC XrBackendOptions xr_backend_default_options(void);
XR_FUNC XrBackendStatus xr_backend_ir_build(const XrValidatedProgram *program,
                                            const XrTargetProfile *profile,
                                            const XrBackendOptions *options, XrBackendIR **ir_out,
                                            XrBackendDiagnostic *diagnostic_out);
XR_FUNC void xr_backend_ir_free(XrBackendIR *ir);
XR_FUNC XrBackendIR *xr_backend_ir_retain(const XrBackendIR *ir);
XR_FUNC bool xr_backend_ir_verify(const XrBackendIR *ir, XrBackendDiagnostic *diagnostic_out);
XR_FUNC bool xr_backend_ir_binding_verify(const XrBackendIR *ir,
                                          XrBackendDiagnostic *diagnostic_out);
XR_FUNC XrExecutionId xr_backend_ir_execution_id(const XrBackendIR *ir);
XR_FUNC XrBackendId xr_backend_ir_backend_id(const XrBackendIR *ir);
XR_FUNC XrOptimizationPolicyId xr_backend_ir_optimization_policy_id(const XrBackendIR *ir);
XR_FUNC XrFingerprint xr_backend_ir_lowering_digest(const XrBackendIR *ir);
XR_FUNC size_t xr_backend_ir_instruction_count(const XrBackendIR *ir);
XR_FUNC XrBackendStatus xr_backend_ir_emit_c(const XrBackendIR *ir, bool standalone_main,
                                             XrGeneratedC *generated_out,
                                             XrBackendDiagnostic *diagnostic_out);
XR_FUNC XrBackendStatus xr_backend_ir_emit_c_exports(const XrBackendIR *ir, bool standalone_main,
                                                     const XrBackendCExport *exports,
                                                     uint32_t export_count,
                                                     XrGeneratedC *generated_out,
                                                     XrBackendDiagnostic *diagnostic_out);
XR_FUNC void xr_generated_c_free(XrGeneratedC *generated);

XR_FUNC const char *xr_backend_status_name(XrBackendStatus status);

#endif  // XR_BACKEND_IR_H
