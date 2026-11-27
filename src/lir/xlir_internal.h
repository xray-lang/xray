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

enum { XR_LIR_ARRAY_FAMILY_COUNT = 5 };
/* One row per family for NEW..REPEAT, SET/PUSH, DISPLAY, CAPACITY/RESERVE and
 * WITH_CAPACITY, plus the U8 COPY_STRING_BYTES and RANDOM_BYTES rows. */
enum { XR_LIR_ARRAY_CATALOG_COUNT = XR_LIR_ARRAY_FAMILY_COUNT * (XR_LIR_ARRAY_OP_RESERVE + 3) + 2 };

static inline bool xr_lir_class_array_element_supported(XrLirType element) {
    return element == XR_LIR_I64 || element == XR_LIR_U8;
}

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
typedef struct XrLirCleanupBlock {
    uint32_t frontier, registration;
    uint32_t *captures, capture_count;
} XrLirCleanupBlock;
typedef struct XrLirBlockData {
    uint32_t *parameters;
    uint32_t parameter_count;
    XrLirInstructionData *instructions;
    uint32_t instruction_count, instruction_capacity;
    bool terminated;
    XrLirCleanupBlock *cleanup;
} XrLirBlockData;

static inline bool xr_lir_function_exit(XrLirOp op) {
    return op == XR_LIR_RET || op == XR_LIR_RETURN_ERROR;
}

/* sink_first aliases the aligned tail of the one may_repeat allocation and
 * exists only for functions with physical cleanup candidates. */
typedef struct XrLirBlockConstructionFacts {
    uint8_t *may_repeat;
    uint32_t *sink_first;
    uint32_t count;
} XrLirBlockConstructionFacts;
typedef struct XrLirFunctionData {
    char *name;
    size_t name_length;
    XrLirType *parameter_types;
    uint32_t *parameters;
    uint32_t parameter_count;
    XrLirType result;
    XrLirType array_result;
    XrLirType error;
    uint32_t initializer_module;
    uint32_t cleanup_owner, cleanup_registration;
    bool has_cleanups, strict_utf8;
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
    uint32_t *block_cleanup_rank; /* Borrowed aligned tail of block_multiplicity. */
    uint32_t block_multiplicity_count;
} XrLirFunctionData;
typedef struct XrLirLiteralData {
    uint8_t *bytes;
    size_t length;
} XrLirLiteralData;
XrLirStatus xr_lir_error_literal(XrLirModule *, XrLirType, XrLirLiteral *);
bool xr_lir_error_literal_matches(const XrLirModule *, XrLirType, uint32_t literal);
typedef struct XrLirTypeData {
    XrLirTypeView shape;
    XrLirTypeLayout layout;
    XrLirCallableType callable;
    XrLirType cell_element; /* Array element for a shaped Cell or Nullable payload. */
    XrLirType class_parent;
    const XrLirRecordField *fields;
    const XrLirSumVariant *variants;
    uint32_t variant_count;
    XrLirRecordIdentity record;
    uint32_t *record_names, record_name_capacity;
    uint32_t inline_array_ordinal;
    uint32_t inline_array_operations[XR_LIR_ARRAY_OP_PUSH];
    bool inline_display;
    bool class_payload_only; /* A row with immutable inline optional integer fields. */
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
    uint32_t inline_array_count;
    uint32_t array_operations[XR_LIR_ARRAY_FAMILY_COUNT][XR_LIR_ARRAY_OP_WITH_CAPACITY];
    uint32_t array_ref_operations[XR_LIR_ARRAY_FAMILY_COUNT][2];
    uint32_t array_ref_random_bytes, array_u8_timing_equal;
    bool needs_array_timing_equal;
    XrLirStatus failure;
    XrLirDiagnostic failure_diagnostic;
    char *failure_path;
    bool finished;
    XrLirStateLayout state_layout;
    uint32_t state_entry;
    uint32_t startup_entry, startup_module_count;
    bool state_layout_set;
    XrLirCallableFacts *callable_facts;
    XrLirSliceFacts *slice_facts;
};

/* Only the source Lower or a verified rebuild supplies this static role and
 * the actual SSA captures from its registration point. */
XR_FUNC XrLirStatus xr_lir_internal_cleanup_block(XrLirModule *, XrLirBlock, uint32_t frontier,
    uint32_t registration, const XrLirValue *captures, uint32_t count);

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
/* Constant branches have one executable edge. Return UINT32_MAX for
 * terminators whose successor is not known from the verified instruction. */
static inline uint32_t xr_lir_internal_known_edge(const XrLirFunctionData *f,
    const XrLirInstructionView *term) {
    if (term->op == XR_LIR_BR) return 0;
    if (term->op != XR_LIR_CONDBR) return UINT32_MAX;
    const XrLirInstructionView *test = xr_lir_internal_value_definition(f, term->operands[0]);
    if (!test || test->op != XR_LIR_CONST || test->type != XR_LIR_I1 || test->bits > 1) return UINT32_MAX;
    return test->bits ? 0 : 1;
}

/* A forwarding block may define scalar constants, but does no work whose
 * result or observable behavior can change between iterations. The caller
 * must separately prove the behavior of every argument on its selected edge. */
static inline uint32_t xr_lir_internal_forward_edge(const XrLirFunctionData *f, const XrLirBlockData *block) {
    const XrLirInstructionView *term = &block->instructions[block->instruction_count - 1].view;
    uint32_t selected = xr_lir_internal_known_edge(f, term);
    if (selected == UINT32_MAX) return selected;
    for (uint32_t n = 0; n + 1 < block->instruction_count; ++n) {
        const XrLirInstructionView *op = &block->instructions[n].view;
        if (op->op != XR_LIR_CONST || (op->type != XR_LIR_I64 && op->type != XR_LIR_I1)) return UINT32_MAX;
    }
    return selected;
}

/* Return the signed strict order implied by a selected conditional edge.
 * This describes operand identities only. It does not prove a recurrence or
 * grant permission to skip any of the original control flow. */
static inline const XrLirInstructionView *xr_lir_internal_branch_i64_lt(
    const XrLirFunctionData *f, const XrLirInstructionView *branch, uint32_t edge, uint32_t order[2]) {
    if (branch->op != XR_LIR_CONDBR || branch->operand_count != 1 || branch->edge_count != 2 || edge >= 2)
        return NULL;
    const XrLirInstructionView *compare;
    uint32_t condition = branch->operands[0], left, right, block = 0;
    for (;;) {
        compare = xr_lir_internal_value_definition(f, condition);
        if (!compare || compare->op != XR_LIR_ICMP || compare->type != XR_LIR_I1 || compare->operand_count != 2)
            return NULL;
        uint32_t current_block = f->values[condition - 1].block;
        if (block && block != current_block) return NULL;
        block = current_block;
        left = compare->operands[0]; right = compare->operands[1];
        if (!left || left > f->value_count || !right || right > f->value_count) return NULL;
        XrLirType type = f->values[left - 1].type;
        if (type != f->values[right - 1].type) return NULL;
        if (type == XR_LIR_I64) break;
        /* An equality of a boolean and a known bit only changes the polarity
         * of its SSA operand. Admission already proves acyclic definitions. */
        if (type != XR_LIR_I1 || (compare->predicate != XR_LIR_EQ && compare->predicate != XR_LIR_NE)) return NULL;
        const XrLirInstructionView *bit = xr_lir_internal_value_definition(f, right);
        condition = left;
        if (!bit || bit->op != XR_LIR_CONST) {
            bit = xr_lir_internal_value_definition(f, left);
            condition = right;
        }
        if (!bit || bit->op != XR_LIR_CONST || bit->type != XR_LIR_I1 || bit->bits > 1) return NULL;
        edge ^= (compare->predicate == XR_LIR_EQ) != (bit->bits != 0);
    }
    if (compare->predicate == (edge ? XR_LIR_GE : XR_LIR_LT)) {
        order[0] = left;
        order[1] = right;
    } else if (compare->predicate == (edge ? XR_LIR_LE : XR_LIR_GT)) {
        order[0] = right;
        order[1] = left;
    } else return NULL;
    return compare;
}
static inline const XrLirTypeData *xr_lir_internal_constructed_data(const XrLirModule *m, XrLirType type) {
    if (!m || type < XR_LIR_TYPE_COUNT || !m->types || m->type_count > m->type_capacity ||
        type - XR_LIR_TYPE_COUNT >= m->type_count) return NULL;
    return &m->types[type - XR_LIR_TYPE_COUNT];
}
XR_FUNC bool xr_lir_internal_type_valid(const XrLirModule *, XrLirType type, bool unit);
XR_FUNC bool xr_lir_internal_types_valid(const XrLirModule *);
static inline bool xr_lir_internal_optional_i64_field(const XrLirModule *m, XrLirType type) {
    XrLirTypeView shape;
    return xr_lir_type_view(m, type, &shape) == XR_LIR_OK &&
        shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_I64;
}
XR_FUNC XrLirStatus xr_lir_internal_payloadless_enum_type(XrLirModule *, const XrLirRecordIdentity *,
    const XrLirSumVariant *, uint32_t variant_count, XrLirType *);
static inline bool xr_lir_internal_class_only_field(const XrLirModule *m, XrLirType type) {
    const XrLirTypeData *data = xr_lir_internal_constructed_data(m, type);
    return xr_lir_internal_optional_i64_field(m, type) ||
        (data && data->shape.kind == XR_LIR_TYPE_SUM && data->record.name &&
            !data->record.argument_count && !data->fields);
}
static inline bool xr_lir_class_scalar_field_supported(const XrLirModule *m, XrLirType type) {
    return type == XR_LIR_I1 || type == XR_LIR_I64 || type == XR_LIR_F64 ||
        xr_lir_internal_class_only_field(m, type);
}
static inline bool xr_lir_class_string_field_supported(const XrLirModule *m, XrLirType type) {
    XrLirTypeView shape;
    return type == XR_LIR_STRING ||
        (xr_lir_type_view(m, type, &shape) == XR_LIR_OK &&
            shape.kind == XR_LIR_TYPE_NULLABLE && shape.element == XR_LIR_STRING);
}
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
XR_FUNC bool xr_lir_internal_array_element(const XrLirModule *, XrLirType);
XR_FUNC uint32_t xr_lir_array_layout_family(const XrLirModule *, XrLirType);
XR_FUNC XrLirType xr_lir_internal_array_element_at(uint32_t family);
XR_FUNC XrLirStatus xr_lir_require_inline_array_element(XrLirModule *, XrLirType);
/* The caller consumes Checked's allocation-cycle result before admitting this shape. */
XR_FUNC XrLirStatus xr_lir_internal_class_type(XrLirModule *, XrLirType payload, XrLirType parent, XrLirType *);
XR_FUNC XrLirStatus xr_lir_internal_named_class_type(XrLirModule *, const XrLirRecordIdentity *,
    const XrLirRecordField *, uint32_t count, XrLirType parent, XrLirType *);
XR_FUNC uint32_t xr_lir_internal_array_family(XrLirType);
static inline bool xr_lir_internal_array_type(XrLirType type) {
    return type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF;
}
XR_FUNC bool xr_lir_internal_array_intrinsic_valid(const XrLirModule *, const XrLirIntrinsic *,
    const XrLirArrayIntrinsic *);
XR_FUNC const XrLirArrayIntrinsic *xr_lir_internal_array_intrinsic(const XrLirModule *, uint32_t);
XR_FUNC XrLirType xr_lir_internal_array_result(const XrLirModule *,
    const XrLirFunctionData *, const XrLirInstructionView *);
XR_FUNC bool xr_lir_internal_array_instruction(const XrLirModule *,
    const XrLirFunctionData *, const XrLirInstructionView *);
XR_FUNC bool xr_lir_internal_state_layout_valid(const XrLirModule *, const XrLirStateLayout *);
XR_FUNC bool xr_lir_internal_state_payload(const XrLirModule *, XrLirType, uint32_t *, uint32_t *);
XR_FUNC bool xr_lir_internal_terminal(XrLirOp op);
XR_FUNC bool xr_lir_internal_invoke_result_type(const XrLirModule *, XrLirType);
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
