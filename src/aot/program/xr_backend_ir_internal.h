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
    XR_BACKEND_VALUE_ERROR_U32,
    XR_BACKEND_VALUE_AGGREGATE,
} XrBackendValueRepresentation;

typedef struct XrBackendInstruction {
    uint16_t operation_id;
    uint16_t result_type_id;
    uint32_t result_id;
    uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
    } immediate;
    uint32_t *successors;
    uint32_t successor_count;
} XrBackendInstruction;

typedef struct XrBackendBlock {
    uint32_t *argument_ids;
    uint16_t *argument_types;
    uint32_t argument_count;
    XrBackendInstruction *instructions;
    uint32_t instruction_count;
} XrBackendBlock;

typedef struct XrBackendFunction {
    uint16_t *parameter_types;
    uint32_t parameter_count;
    uint16_t result_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    uint32_t entry_block;
    XrBackendBlock *blocks;
    uint32_t block_count;
    uint16_t *value_types;
    uint8_t *value_representations;
    uint32_t value_count;
    uint32_t flags;
} XrBackendFunction;

struct XrBackendIR {
    XrValidatedProgram *program;
    XrTargetProfile *profile;
    XrExecutionId execution_id;
    XrBackendId backend_id;
    XrOptimizationPolicyId optimization_policy_id;
    XrFingerprint lowering_digest;
    XrBackendOptions options;
    XrValidatedConstant *constants;
    uint32_t constant_count;
    XrBackendFunction *functions;
    uint32_t function_count;
    uint32_t entry_function;
    uint32_t pointer_width;
    size_t instruction_count;
    bool verified;
};

XR_FUNC bool xr_backend_representation_for_type(uint16_t type_id, uint8_t *representation_out);
XR_FUNC XrBackendId xr_backend_compute_id(void);
XR_FUNC XrOptimizationPolicyId
xr_backend_compute_optimization_policy_id(const XrBackendOptions *options);
XR_FUNC void xr_backend_compute_lowering_digest(const XrBackendIR *ir, XrFingerprint *digest_out);
XR_FUNC void xr_backend_set_diagnostic(XrBackendDiagnostic *diagnostic, XrBackendStatus status,
                                       uint16_t operation_id, uint32_t function_id,
                                       uint32_t block_id, uint32_t instruction_id);

#endif  // XR_BACKEND_IR_INTERNAL_H
