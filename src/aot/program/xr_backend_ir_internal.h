/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_internal.h - Owned storage for private AOT realization
 */

#ifndef XR_BACKEND_IR_INTERNAL_H
#define XR_BACKEND_IR_INTERNAL_H

#include "xr_backend_ir.h"
#include "../../program/xr_validated_program_internal.h"

typedef enum XrBackendValueRepresentation {
    XR_BACKEND_VALUE_VOID = 0,
    XR_BACKEND_VALUE_BOOL_U8,
    XR_BACKEND_VALUE_I64,
    XR_BACKEND_VALUE_U32,
    XR_BACKEND_VALUE_U16,
    XR_BACKEND_VALUE_TARGET_ENUM_U16,
    XR_BACKEND_VALUE_ERROR_U32,
    XR_BACKEND_VALUE_PANIC_U32,
    XR_BACKEND_VALUE_AGGREGATE,
    XR_BACKEND_VALUE_CLASS_HANDLE,
    XR_BACKEND_VALUE_STRING_HANDLE,
    XR_BACKEND_VALUE_RUNE_U32,
    XR_BACKEND_VALUE_I8,
    XR_BACKEND_VALUE_U8,
    XR_BACKEND_VALUE_I16,
    XR_BACKEND_VALUE_I32,
    XR_BACKEND_VALUE_U64,
    XR_BACKEND_VALUE_F64,
} XrBackendValueRepresentation;

struct XrBackendIR {
    atomic_uint_least32_t references;
    XrValidatedProgram *program;
    XrTargetProfile *profile;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint lowering_digest;
    XrBackendOptions options;
    uint16_t pointer_width;
    uint16_t operating_system;
    uint16_t architecture;
    uint16_t native_abi;
    uint16_t endianness;
    size_t instruction_count;
    bool verified;
};

XR_FUNC bool xr_backend_representation_for_type(uint16_t type_id, uint8_t *representation_out);
XR_FUNC bool xr_backend_representation_for_program_type(const XrValidatedProgram *program,
                                                         uint16_t type_id,
                                                         uint8_t *representation_out);
XR_FUNC XrBackendId xr_backend_compute_id(void);
XR_FUNC XrOptimizationPolicyId
xr_backend_compute_optimization_policy_id(const XrBackendOptions *options);
XR_FUNC void xr_backend_compute_lowering_digest(const XrBackendIR *ir, XrFingerprint *digest_out);
XR_FUNC void xr_backend_set_diagnostic(XrBackendDiagnostic *diagnostic, XrBackendStatus status,
                                       uint16_t operation_id, uint32_t function_id,
                                       uint32_t block_id, uint32_t instruction_id);

#endif  // XR_BACKEND_IR_INTERNAL_H
