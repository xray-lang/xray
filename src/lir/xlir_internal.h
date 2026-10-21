/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_internal.h - Module-owned construction storage
 *
 * KEY CONCEPT:
 *   Local indices survive vector growth. Pointers to vector elements are
 *   temporary implementation details and never construction handles.
 */
#ifndef XLIR_INTERNAL_H
#define XLIR_INTERNAL_H
#include "xlir_builder.h"

typedef struct XrLirValueData {
    XrLirType type;
    uint32_t block, instruction;
    bool parameter;
} XrLirValueData;
typedef struct XrLirInstructionData {
    XrLirInstructionView view;
} XrLirInstructionData;
typedef struct XrLirBlockData {
    uint32_t *parameters;
    uint32_t parameter_count;
    XrLirInstructionData *instructions;
    uint32_t instruction_count, instruction_capacity;
    bool terminated;
} XrLirBlockData;
typedef struct XrLirFunctionData {
    char *name;
    size_t name_length;
    XrLirType *parameter_types;
    uint32_t *parameters;
    uint32_t parameter_count;
    XrLirType result;
    XrLirFunctionKind kind;
    XrLirExternKind extern_kind;
    bool defined;
    uint32_t entry;
    XrLirBlockData *blocks;
    uint32_t block_count, block_capacity;
    XrLirValueData *values;
    uint32_t value_count, value_capacity;
} XrLirFunctionData;
struct XrLirModule {
    XrLirFunctionData *functions;
    uint32_t function_count, function_capacity;
    XrLirIntrinsic *intrinsics;
    uint32_t intrinsic_count;
    XrLirStatus failure;
    XrLirDiagnostic failure_diagnostic;
    char *failure_path;
    bool finished;
};

XR_FUNC bool xr_lir_internal_type_valid(XrLirType type, bool unit);
XR_FUNC bool xr_lir_internal_terminal(XrLirOp op);
XR_FUNC XrLirStatus xr_lir_internal_instruction_check(const XrLirModule *, uint32_t function,
                                                      const XrLirInstructionView *,
                                                      XrLirDiagnostic *);
XR_FUNC XrLirStatus xr_lir_internal_error(XrLirDiagnostic *, const char *message, uint32_t function,
                                          uint32_t block, uint32_t instruction,
                                          const XrLirSource *);

#endif  // XLIR_INTERNAL_H
