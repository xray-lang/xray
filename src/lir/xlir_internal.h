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
#include "xlir_view.h"
#include "xlir_display.h"
#include "xlir_callable.h"
#include "xlir_cell.h"
#include "xlir_string.h"
#include "xlir_range.h"
#include "xlir_map.h"
#include "xlir_ownership.h"
#include "xlir_callable_facts.h"
#include "xlir_slice_facts.h"

enum { XR_LIR_ARRAY_FAMILY_COUNT = 4 };
enum { XR_LIR_ARRAY_CATALOG_COUNT = XR_LIR_ARRAY_FAMILY_COUNT * (XR_LIR_ARRAY_OP_REPEAT + 3) };

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
    XrLirClosureTarget closure;
    XrLirFunctionCallableParameter *callable_parameters;
    uint32_t callable_parameter_count;
    XrLirFunctionKind kind;
    XrLirExternKind extern_kind;
    bool defined;
    uint32_t entry;
    XrLirBlockData *blocks;
    uint32_t block_count, block_capacity;
    XrLirValueData *values;
    uint32_t value_count, value_capacity;
    uint32_t scope_count;
    uint8_t *block_multiplicity; /* Owned only after complete immutable admission. */
    uint32_t block_multiplicity_count;
} XrLirFunctionData;
typedef struct XrLirLiteralData {
    uint8_t *bytes;
    size_t length;
} XrLirLiteralData;
typedef struct XrLirTypeData {
    XrLirTypeView shape;
    XrLirTypeLayout layout;
    XrLirCallableType callable;
    XrLirType cell_element;
    const XrLirRecordField *fields;
    XrLirRecordIdentity record;
    uint32_t *record_names, record_name_capacity;
} XrLirTypeData;
struct XrLirModule {
    XrLirTypeData *types;
    uint32_t type_count, type_capacity;
    uint32_t *type_index, type_index_capacity;
    XrLirLiteralData *literals;
    uint32_t literal_count, literal_capacity;
    XrLirMapLiteral *map_literals;
    uint32_t map_literal_count,map_literal_capacity;
    XrLirFunctionData *functions;
    uint32_t function_count, function_capacity;
    XrLirIntrinsic *intrinsics;
    uint32_t intrinsic_count;
    XrLirArrayIntrinsic *array_intrinsics;
    uint32_t array_operations[XR_LIR_ARRAY_FAMILY_COUNT][XR_LIR_ARRAY_OP_DISPLAY];
    uint32_t array_ref_operations[XR_LIR_ARRAY_FAMILY_COUNT][2];
    XrLirStatus failure;
    XrLirDiagnostic failure_diagnostic;
    char *failure_path;
    bool finished;
    XrLirStateLayout state_layout;
    uint32_t state_entry;
    bool state_layout_set;
    XrLirCallableFacts *callable_facts;
    XrLirSliceFacts *slice_facts;
};

/* Only Lower receives these numeric contracts from the exact completed Source
 * artifact. Rebuilding copies them with actual function relocation; neither
 * backend supplies summaries or treats absent terms as an empty WRITE set. */
XR_FUNC XrLirStatus xr_lir_internal_slice_effects_set(XrLirModule *, XrLirSliceFactsView);
XR_FUNC XrLirStatus xr_lir_internal_slice_effects_copy(XrLirModule *, const XrLirModule *,
    const XrLirFunction *functions, uint32_t count);
XR_FUNC XrLirStatus xr_lir_internal_slice_function_fact(const XrLirModule *, uint32_t,
    XrLirSliceFunctionFact *);

XR_FUNC bool xr_lir_internal_slice_intrinsic_valid(const XrLirModule *, const XrLirIntrinsic *);
/* The receiving catalog exists before function construction. This borrowed
 * entry stays with its module; copy the needed signature fields before emit. */
XR_FUNC const XrLirIntrinsic *xr_lir_internal_slice_intrinsic(const XrLirModule *, uint32_t);
/* Insert a fully resolved non-reading cleanup after ordinary SSA construction.
 * The standard emitter owns its source and validates its target; this helper
 * preserves all existing definitions while restoring the intended position. */
XR_FUNC XrLirStatus xr_lir_internal_slice_cleanup_insert(XrLirModule *, XrLirBlock,
    uint32_t ordinal, XrLirValue target, const XrLirSource *);
typedef struct XrLirSliceCleanupInsertion {
    XrLirBlock block;
    uint32_t ordinal;
    XrLirValue target;
    XrLirSource source;
    /* Zero retains the local keeper cleanup. Formal cleanup is a distinct
     * resource effect with no physical keeper or ordinary operand read. */
    bool formal;
} XrLirSliceCleanupInsertion;
/* Input positions refer to the unchanged body and are ordered within each
 * block. Each block is merged once after the normal emitter owns its effects. */
XR_FUNC XrLirStatus xr_lir_internal_slice_cleanup_batch(XrLirModule *, XrLirFunction,
    const XrLirSliceCleanupInsertion *, size_t count);
XR_FUNC bool xr_lir_internal_slice_type(const XrLirModule *, XrLirType);

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
XR_FUNC bool xr_lir_internal_map_literals_valid(const XrLirModule *);
XR_FUNC bool xr_lir_internal_closure_target_valid(const XrLirModule *, const XrLirFunctionData *);
XR_FUNC bool xr_lir_internal_callable_parameters_valid(const XrLirModule *, const XrLirType *, uint32_t,
    const XrLirFunctionCallableParameter *, uint32_t, bool);
XR_FUNC bool xr_lir_internal_callable_instruction(const XrLirModule *,
    const XrLirFunctionData *, const XrLirInstructionView *);
XR_FUNC bool xr_lir_internal_callable_persistent(const XrLirModule *, const XrLirFunctionData *, uint32_t);
/* The constructor receives facts from the same scope-verification pass. This
 * is not an alternate admission path or a caller-selectable trust flag. */
XR_FUNC XrLirStatus xr_lir_internal_finish_verify(XrLirModule *, XrLirDiagnostic *);
XR_FUNC void xr_lir_internal_block_multiplicity_free(uint8_t *);
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
/* Cold backend collection immediately follows successful verification of this
 * exact unchanged finished module. Public plan_create still revalidates all IR.
 * This internal entry copies events only; it grants no admission or trust flag.
 * The supplied constructed facts still come from the common value producer. */
XR_FUNC XrLirStatus xr_lir_internal_ownership_collect(const XrLirOwnershipRequest *,
    XrLirOwnershipPlan **empty, XrLirDiagnostic *);
XR_FUNC XrLirStatus xr_lir_internal_instruction_check(const XrLirModule *, uint32_t function,
                                                      const XrLirInstructionView *,
                                                      XrLirDiagnostic *);
XR_FUNC XrLirStatus xr_lir_internal_error(XrLirDiagnostic *, const char *message, uint32_t function,
                                          uint32_t block, uint32_t instruction,
                                          const XrLirSource *);

#endif  // XLIR_INTERNAL_H
