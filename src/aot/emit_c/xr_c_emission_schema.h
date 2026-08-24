/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_schema.h - Self-contained dumb C emitter value schema
 *
 * KEY CONCEPT:
 *   This header contains only immutable C emission records. A text emitter can
 *   consume it without access to semantic plans, compiler IR, analyzer types,
 *   or the legacy AOT representation model. TAGGED is the exact C projection
 *   of a verified DYN_VALUE row; it is not inferred from a source type.
 */

#ifndef XR_C_EMISSION_SCHEMA_H
#define XR_C_EMISSION_SCHEMA_H

#include <stdbool.h>
#include <stdint.h>

typedef enum XrCValueRep {
    XR_C_VALUE_REP_VOID = 0,
    XR_C_VALUE_REP_I8,
    XR_C_VALUE_REP_U8,
    XR_C_VALUE_REP_I16,
    XR_C_VALUE_REP_U16,
    XR_C_VALUE_REP_I32,
    XR_C_VALUE_REP_U32,
    XR_C_VALUE_REP_I64,
    XR_C_VALUE_REP_U64,
    XR_C_VALUE_REP_ISIZE,
    XR_C_VALUE_REP_USIZE,
    XR_C_VALUE_REP_F32,
    XR_C_VALUE_REP_F64,
    XR_C_VALUE_REP_BOOL,
    XR_C_VALUE_REP_RUNE,
    XR_C_VALUE_REP_TAGGED,
    XR_C_VALUE_REP_VIEW,
    XR_C_VALUE_REP_AGGREGATE,
    XR_C_VALUE_REP_RAW_PTR,
    XR_C_VALUE_REP_COUNT,
} XrCValueRep;

typedef enum XrCValueMaterializationRecipe {
    XR_C_VALUE_MATERIALIZATION_NONE = 0,
    XR_C_VALUE_MATERIALIZATION_STRING_LITERAL_VIEW = 1,
    XR_C_VALUE_MATERIALIZATION_CHANNEL_NEW = 2,
    XR_C_VALUE_MATERIALIZATION_CHANNEL_RECV_PAYLOAD = 3,
    XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_NEW = 4,
    XR_C_VALUE_MATERIALIZATION_STRING_BYTE_SLICE_VIEW = 5,
    XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_RUNE = 6,
    XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_TO_STRING = 7,
    XR_C_VALUE_MATERIALIZATION_STRINGBUILDER_APPEND_STRING = 8,
    XR_C_VALUE_MATERIALIZATION_STRING_CONCAT = 9,
    XR_C_VALUE_MATERIALIZATION_PANIC_CATCH = 10,
    XR_C_VALUE_MATERIALIZATION_ADT_ENUM_CONSTRUCTOR = 11,
    XR_C_VALUE_MATERIALIZATION_ARRAY_WITH_CAPACITY = 12,
    XR_C_VALUE_MATERIALIZATION_ARRAY_FILLED_NEW = 13,
    XR_C_VALUE_MATERIALIZATION_STRING_RUNES = 14,
    XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_HAS_NEXT = 15,
    XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NEXT = 16,
    XR_C_VALUE_MATERIALIZATION_SCALAR_ADDRESSABLE_ALIAS = 17,
    XR_C_VALUE_MATERIALIZATION_DIRECT_LOCAL_TAGGED_REF_PARAMETER = 18,
    XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32 = 19,
    XR_C_VALUE_MATERIALIZATION_RUNE_IS_WHITESPACE = 20,
    XR_C_VALUE_MATERIALIZATION_ARRAY_NEW = 21,
    XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE = 22,
    XR_C_VALUE_MATERIALIZATION_ARRAY_FILL_SCALAR = 23,
    XR_C_VALUE_MATERIALIZATION_ARRAY_HOF_DIRECT = 24,
    XR_C_VALUE_MATERIALIZATION_LOCAL_ADDRESS = 25,
    XR_C_VALUE_MATERIALIZATION_ITERATOR_RUNE_NTH = 26,
    XR_C_VALUE_MATERIALIZATION_RUNE_TO_STRING = 27,
    XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED = 28,
    XR_C_VALUE_MATERIALIZATION_COUNT,
} XrCValueMaterializationRecipe;

typedef enum XrCRecipeArgumentKind {
    XR_C_RECIPE_ARGUMENT_INVALID = 0,
    XR_C_RECIPE_ARGUMENT_STRING_VALUE = 1,
    XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD = 2,
    XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND = 3,
    XR_C_RECIPE_ARGUMENT_STRING_DIRECT_U64 = 4,
    XR_C_RECIPE_ARGUMENT_ARRAY_HOF_RECEIVER = 5,
    XR_C_RECIPE_ARGUMENT_ARRAY_HOF_CALLBACK = 6,
    XR_C_RECIPE_ARGUMENT_ARRAY_HOF_SEED = 7,
    XR_C_RECIPE_ARGUMENT_STRING_DIRECT_I64 = 8,
} XrCRecipeArgumentKind;

/* A concatenation piece the emitter formats straight from its native integer,
 * with no tagged value in between.  Both widths reach the runtime the same way
 * and differ only in which formatter reads them, so every site that asks "is
 * this piece direct" asks here rather than naming the widths itself. */
static inline bool xr_c_recipe_argument_is_direct_scalar(int kind) {
    return kind == XR_C_RECIPE_ARGUMENT_STRING_DIRECT_U64 ||
           kind == XR_C_RECIPE_ARGUMENT_STRING_DIRECT_I64;
}

typedef enum XrCArrayHofKind {
    XR_C_ARRAY_HOF_NONE = 0,
    XR_C_ARRAY_HOF_MAP,
    XR_C_ARRAY_HOF_FILTER,
    XR_C_ARRAY_HOF_REDUCE,
    XR_C_ARRAY_HOF_COUNT,
} XrCArrayHofKind;

typedef enum XrCAddressProjection {
    XR_C_ADDRESS_PROJECTION_NONE = 0,
    XR_C_ADDRESS_PROJECTION_NAMED_AGGREGATE = 1,
    XR_C_ADDRESS_PROJECTION_FIXED_ARRAY_BACKING = 2,
    XR_C_ADDRESS_PROJECTION_TUPLE_BACKING = 3,
} XrCAddressProjection;

typedef enum XrCCleanupAction {
    XR_C_CLEANUP_INVALID = 0,
    XR_C_CLEANUP_RELEASE = 1,
} XrCCleanupAction;

typedef struct XrCRecipeArgumentView {
    /* Ordered logical operand identity and the exact frozen value consumed by
     * the C recipe.  They are intentionally separate: representation lowering
     * may insert a backend-only adapter between them after plan freeze. */
    uint32_t semantic_value;
    uint32_t source_semantic_value;
    uint8_t kind;
    uint8_t reserved[3];
} XrCRecipeArgumentView;

typedef struct XrCValueEmissionView {
    uint32_t semantic_value;
    uint16_t target_register_rep;
    uint16_t target_memory_rep;
    uint16_t target_register_kind;
    uint16_t target_memory_kind;
    uint16_t register_bits;
    uint16_t memory_align;
    uint32_t memory_size;
    uint8_t rep;
    uint8_t materialization;
    uint16_t reserved;
    uint32_t literal_byte_length;
    uint32_t recipe_operand_value;
    uint32_t recipe_argument_value;
    uint32_t recipe_layout_id;
    uint32_t recipe_discriminant;
    uint16_t recipe_argument_count;
    uint16_t recipe_reserved;
    uint32_t recipe_callee_function;
    uint8_t recipe_hof_kind;
    uint8_t recipe_hof_source_storage;
    uint8_t recipe_hof_result_storage;
    uint8_t recipe_hof_callback_parameter_reps[2];
    uint8_t recipe_hof_callback_return_rep;
    uint8_t recipe_hof_reserved;
    uint32_t backing_value;
    uint32_t backing_element_count;
    uint8_t address_projection;
    uint8_t backing_native_type;
    uint16_t projection_reserved;
    const char *c_type;
    const char *backing_c_type;
    const char *literal_bytes;
    const char *recipe_symbol;
    const char *recipe_type_name;
    const char *recipe_member_name;
    const XrCRecipeArgumentView *recipe_arguments;
} XrCValueEmissionView;

/* Exact C boundary for one direct-local ref Array argument.  This row is
 * keyed by the semantic CALL result and parameter ordinal; it deliberately
 * carries no selector, source name, analyzer type, or mutable Xi pointer. */
typedef struct XrCCallArgumentEmissionView {
    uint32_t semantic_call_value;
    uint32_t semantic_operand;
    uint32_t semantic_value;
    uint32_t callee_parameter;
    uint16_t ordinal;
    uint16_t caller_register_kind;
    uint16_t caller_memory_kind;
    uint16_t callee_register_kind;
    uint16_t callee_memory_kind;
    uint8_t mode;
    uint8_t ownership;
    uint8_t transfer_mode;
    uint8_t flags;
    uint8_t array_element_storage;
    uint8_t reserved[3];
    const char *c_type;
} XrCCallArgumentEmissionView;

/* What an aggregate slot aggregates. The C representation says only that a
 * slot is an aggregate; the emitter needs to know which one, and the frozen
 * semantic type already says so, so the projection carries the answer rather
 * than leaving the emitter to recognise a C spelling. */
typedef enum XrCAbiAggregateClass {
    XR_C_ABI_AGGREGATE_NONE = 0,
    XR_C_ABI_AGGREGATE_STRUCT = 1,
    XR_C_ABI_AGGREGATE_ADT_ENUM = 2,
    XR_C_ABI_AGGREGATE_SLICE = 3,
} XrCAbiAggregateClass;

typedef enum XrCAbiSlotClass {
    XR_C_ABI_SLOT_INVALID = 0,
    /* Passed as its own C value. */
    XR_C_ABI_SLOT_VALUE = 1,
    /* Passed as a pointer to a caller-owned place the callee only reads or
     * writes through; `pointee_rep`/`pointee_c_type` describe what it points
     * at, and the pointer itself is never the value. */
    XR_C_ABI_SLOT_BORROWED_PLACE = 2,
} XrCAbiSlotClass;

/* Exact C boundary for one slot of one function signature: ordinal 0 is the
 * return, ordinals 1..N the parameters in declaration order. One row per slot
 * rather than a nested signature keeps the table flat and binary-searchable
 * like every other emission table.
 *
 * The target plan has no signature of its own -- its function record carries a
 * frame, not an ABI -- so these rows are assembled from the frozen semantic
 * parameter list and the target value representation each of those subjects
 * already has. That is the same pair the value rows are built from, which is
 * what keeps a signature and the values crossing it from disagreeing.
 *
 * `c_type` is present for the same reason it is on the value row: this plan is
 * the C-specific projection, and a text emitter must not have to reach for the
 * old representation model to spell a parameter. */
/* How a function's boundary reaches C.
 *
 * A property of the function, not a consequence of its slots: a module
 * initializer takes the tagged boundary however its slots are shaped, and so
 * do a capturing function and a coroutine. Deriving it from the slot rows was
 * measured and disagreed on 318 of 582 functions, every one an initializer. */
/* Verification status, stated because the three cases are not equally proven:
 * only NATIVE has been measured against the older per-function record (582
 * comparisons, zero disagreements). TAGGED and COROUTINE are *not* drop-in
 * replacements for that record's XAOT_ABI_TAGGED / XAOT_ABI_CORO -- this
 * enum separates them by whether the function can suspend, while the older
 * record separates them by whether the function carries coroutine operations.
 * A module initializer that can suspend lands in COROUTINE here and in TAGGED
 * there. Ask `!= NATIVE` when that is the question; do not read COROUTINE as
 * "the older model would have said CORO" until that case is measured too. */
typedef enum XrCAbiBoundaryKind {
    XR_C_ABI_BOUNDARY_INVALID = 0,
    /* Every slot travels in the carrier its own type admits. */
    XR_C_ABI_BOUNDARY_NATIVE = 1,
    /* The whole signature travels tagged. */
    XR_C_ABI_BOUNDARY_TAGGED = 2,
    /* A coroutine: tagged, and the frame is entered through a resume. */
    XR_C_ABI_BOUNDARY_COROUTINE = 3,
} XrCAbiBoundaryKind;

typedef struct XrCFunctionAbiEmissionView {
    uint32_t semantic_function;
    uint32_t semantic_value;
    uint16_t ordinal;
    uint16_t parameter_count;
    uint16_t target_register_kind;
    uint16_t target_memory_kind;
    uint8_t slot_class;
    /* Same on every row of one signature; see XrCAbiBoundaryKind. */
    uint8_t boundary_kind;
    uint8_t rep;
    uint8_t pointee_rep;
    uint8_t aggregate_class;
    const char *c_type;
    const char *pointee_c_type;
} XrCFunctionAbiEmissionView;

/* Exact C projection of one TargetPlan cleanup.  The operation and slot are
 * frozen numeric identities; recipe_symbol is the only emitter spelling. */
typedef struct XrCCleanupEmissionView {
    uint32_t semantic_operation;
    uint32_t semantic_value;
    uint32_t target_slot;
    uint8_t action;
    uint8_t flags;
    uint16_t reserved;
    const char *recipe_symbol;
} XrCCleanupEmissionView;

#endif  // XR_C_EMISSION_SCHEMA_H
