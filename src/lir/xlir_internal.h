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
#include "xlir_state.h"
#include "xlir_array.h"
#include "xlir_types.h"

enum { XR_LIR_ARRAY_FAMILY_COUNT = 4 };

typedef struct XrLirValueData {
    XrLirType type;
    uint32_t block, instruction;
    bool parameter;
    XrLirStateReference state_reference;
    XrLirType array_element;
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
    XrLirType array_result;
    XrLirFunctionKind kind;
    XrLirExternKind extern_kind;
    bool defined;
    uint32_t entry;
    XrLirBlockData *blocks;
    uint32_t block_count, block_capacity;
    XrLirValueData *values;
    uint32_t value_count, value_capacity;
} XrLirFunctionData;
typedef struct XrLirLiteralData {
    uint8_t *bytes;
    size_t length;
} XrLirLiteralData;
typedef struct XrLirTypeData {
    XrLirTypeView shape;
    XrLirTypeLayout layout;
} XrLirTypeData;
struct XrLirModule {
    XrLirTypeData *types;
    uint32_t type_count, type_capacity;
    uint32_t *type_index, type_index_capacity;
    XrLirLiteralData *literals;
    uint32_t literal_count, literal_capacity;
    XrLirFunctionData *functions;
    uint32_t function_count, function_capacity;
    XrLirIntrinsic *intrinsics;
    uint32_t intrinsic_count;
    XrLirArrayIntrinsic *array_intrinsics;
    uint32_t array_operations[XR_LIR_ARRAY_FAMILY_COUNT][XR_LIR_ARRAY_OP_REPEAT];
    uint32_t array_ref_operations[XR_LIR_ARRAY_FAMILY_COUNT][2];
    XrLirStatus failure;
    XrLirDiagnostic failure_diagnostic;
    char *failure_path;
    bool finished;
    XrLirStateLayout state_layout;
    uint32_t state_entry;
    bool state_layout_set;
};

/* Operand validation may reach a later block before its shape is checked. */
static inline const XrLirInstructionView *xr_lir_internal_value_definition(
    const XrLirFunctionData *f, uint32_t id) {
    if (!f || !id || id > f->value_count || f->value_count > f->value_capacity ||
        !f->values || !f->blocks || f->block_count > f->block_capacity)
        return NULL;
    const XrLirValueData *value = &f->values[id - 1];
    if (value->parameter || !value->block || value->block > f->block_count)
        return NULL;
    const XrLirBlockData *block = &f->blocks[value->block - 1];
    if (!block->instructions || block->instruction_count > block->instruction_capacity ||
        value->instruction >= block->instruction_count)
        return NULL;
    const XrLirInstructionView *definition = &block->instructions[value->instruction].view;
    return definition->result == id ? definition : NULL;
}
XR_FUNC bool xr_lir_internal_type_valid(const XrLirModule *, XrLirType type, bool unit);
XR_FUNC bool xr_lir_internal_types_valid(const XrLirModule *);
XR_FUNC bool xr_lir_internal_array_element(XrLirType);
XR_FUNC uint32_t xr_lir_internal_array_family(XrLirType);
static inline bool xr_lir_internal_array_type(XrLirType type) {
    return type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF;
}
XR_FUNC bool xr_lir_internal_array_intrinsic_valid(const XrLirIntrinsic *,
    const XrLirArrayIntrinsic *);
XR_FUNC const XrLirArrayIntrinsic *xr_lir_internal_array_intrinsic(const XrLirModule *, uint32_t);
XR_FUNC XrLirType xr_lir_internal_array_result(const XrLirModule *,
    const XrLirFunctionData *, const XrLirInstructionView *);
XR_FUNC bool xr_lir_internal_array_instruction(const XrLirModule *,
    const XrLirFunctionData *, const XrLirInstructionView *);
XR_FUNC bool xr_lir_internal_state_layout_valid(const XrLirModule *, const XrLirStateLayout *);
XR_FUNC bool xr_lir_internal_state_payload(const XrLirModule *, XrLirType, uint32_t *, uint32_t *);
XR_FUNC bool xr_lir_internal_terminal(XrLirOp op);
XR_FUNC XrLirStatus xr_lir_internal_instruction_check(const XrLirModule *, uint32_t function,
                                                      const XrLirInstructionView *,
                                                      XrLirDiagnostic *);
XR_FUNC XrLirStatus xr_lir_internal_error(XrLirDiagnostic *, const char *message, uint32_t function,
                                          uint32_t block, uint32_t instruction,
                                          const XrLirSource *);

#endif  // XLIR_INTERNAL_H
