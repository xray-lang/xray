/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program.h - Compiler-private CoreIR and canonical XrProgram writer
 */

#ifndef XR_PROGRAM_H
#define XR_PROGRAM_H

#include "../base/xdefs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_PROGRAM_DIGEST_SIZE 32u
#define XR_CORE_IR_KEY_SIZE 32u
#define XR_PROGRAM_FUNCTION_ENTRY UINT32_C(1)

typedef struct XrCoreIrKey {
    uint8_t bytes[XR_CORE_IR_KEY_SIZE];
} XrCoreIrKey;

typedef enum XrCoreIrConstantKind {
    XR_CORE_IR_CONSTANT_I64 = 1,
    XR_CORE_IR_CONSTANT_BOOL = 2,
} XrCoreIrConstantKind;

typedef struct XrCoreIrConstantInput {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrConstantKind kind;
    union {
        int64_t i64;
        bool boolean;
    } value;
} XrCoreIrConstantInput;

typedef enum XrCoreIrImmediateKind {
    XR_CORE_IR_IMMEDIATE_NONE = 0,
    XR_CORE_IR_IMMEDIATE_I64,
    XR_CORE_IR_IMMEDIATE_U32,
    XR_CORE_IR_IMMEDIATE_BOOL,
    XR_CORE_IR_IMMEDIATE_CONSTANT,
    XR_CORE_IR_IMMEDIATE_FUNCTION,
} XrCoreIrImmediateKind;

typedef struct XrCoreIrValueInput {
    XrCoreIrKey key;
    uint16_t type_id;
} XrCoreIrValueInput;

typedef struct XrCoreIrInstructionInput {
    uint16_t operation_id;
    XrCoreIrKey result;
    uint16_t result_type_id;
    const XrCoreIrKey *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        XrCoreIrKey key;
    } immediate;
    const XrCoreIrKey *successors;
    uint32_t successor_count;
} XrCoreIrInstructionInput;

typedef struct XrCoreIrBlockInput {
    XrCoreIrKey key;
    const XrCoreIrValueInput *arguments;
    uint32_t argument_count;
    const XrCoreIrInstructionInput *instructions;
    uint32_t instruction_count;
} XrCoreIrBlockInput;

typedef struct XrCoreIrFunctionInput {
    XrCoreIrKey key;
    const uint16_t *parameter_types;
    uint32_t parameter_count;
    uint16_t result_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    XrCoreIrKey entry_block;
    const XrCoreIrBlockInput *blocks;
    uint32_t block_count;
    uint32_t flags;
} XrCoreIrFunctionInput;

typedef struct XrCoreIrModuleInput {
    XrCoreIrKey key;
    const XrCoreIrConstantInput *constants;
    uint32_t constant_count;
    const XrCoreIrFunctionInput *functions;
    uint32_t function_count;
} XrCoreIrModuleInput;

typedef struct XrCoreIrProgramInput {
    const uint8_t *semantic_profile_fingerprint;
    const uint16_t *required_features;
    uint32_t required_feature_count;
    const XrCoreIrModuleInput *modules;
    uint32_t module_count;
} XrCoreIrProgramInput;

typedef struct XrCoreIrProgram XrCoreIrProgram;

typedef struct XrProgramId {
    uint8_t bytes[XR_PROGRAM_DIGEST_SIZE];
} XrProgramId;

typedef struct XrProgramArtifact {
    uint8_t *bytes;
    size_t size;
    XrProgramId id;
} XrProgramArtifact;

typedef enum XrProgramBuildStatus {
    XR_PROGRAM_BUILD_OK = 0,
    XR_PROGRAM_BUILD_INVALID_INPUT,
    XR_PROGRAM_BUILD_UNSUPPORTED_FEATURE,
    XR_PROGRAM_BUILD_DUPLICATE_IDENTITY,
    XR_PROGRAM_BUILD_UNRESOLVED_REFERENCE,
    XR_PROGRAM_BUILD_RESOURCE_LIMIT,
    XR_PROGRAM_BUILD_OUT_OF_MEMORY,
} XrProgramBuildStatus;

XR_FUNC XrCoreIrKey xr_core_ir_key(const void *semantic_bytes, size_t semantic_size);
XR_FUNC bool xr_core_ir_key_equal(XrCoreIrKey left, XrCoreIrKey right);
XR_FUNC bool xr_core_ir_key_is_zero(XrCoreIrKey key);

XR_FUNC XrProgramBuildStatus xr_core_ir_program_build(const XrCoreIrProgramInput *input,
                                                      XrCoreIrProgram **program_out,
                                                      char *diagnostic, size_t diagnostic_size);
XR_FUNC void xr_core_ir_program_free(XrCoreIrProgram *program);

XR_FUNC XrProgramBuildStatus xr_program_write(const XrCoreIrProgram *program,
                                              XrProgramArtifact *artifact_out, char *diagnostic,
                                              size_t diagnostic_size);
XR_FUNC void xr_program_artifact_free(XrProgramArtifact *artifact);
XR_FUNC void xr_program_compute_id(const uint8_t *bytes, size_t size, XrProgramId *id_out);
XR_FUNC bool xr_program_id_equal(XrProgramId left, XrProgramId right);
XR_FUNC void xr_program_id_hex(XrProgramId id, char output[XR_PROGRAM_DIGEST_SIZE * 2u + 1u]);
XR_FUNC const char *xr_program_build_status_name(XrProgramBuildStatus status);

#endif /* XR_PROGRAM_H */
