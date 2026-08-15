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
    XR_C_VALUE_MATERIALIZATION_DIRECT_LOCAL_ARRAY_REF_PARAMETER = 18,
    XR_C_VALUE_MATERIALIZATION_RUNE_TO_UINT32 = 19,
    XR_C_VALUE_MATERIALIZATION_RUNE_IS_WHITESPACE = 20,
    XR_C_VALUE_MATERIALIZATION_ARRAY_NEW = 21,
    XR_C_VALUE_MATERIALIZATION_STRING_SLICE_RANGE = 22,
    XR_C_VALUE_MATERIALIZATION_ARRAY_FILL_SCALAR = 23,
    XR_C_VALUE_MATERIALIZATION_COUNT,
} XrCValueMaterializationRecipe;

typedef enum XrCRecipeArgumentKind {
    XR_C_RECIPE_ARGUMENT_INVALID = 0,
    XR_C_RECIPE_ARGUMENT_STRING_VALUE = 1,
    XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD = 2,
    XR_C_RECIPE_ARGUMENT_STRING_SLICE_BOUND = 3,
} XrCRecipeArgumentKind;

typedef enum XrCAddressProjection {
    XR_C_ADDRESS_PROJECTION_NONE = 0,
    XR_C_ADDRESS_PROJECTION_NAMED_AGGREGATE = 1,
    XR_C_ADDRESS_PROJECTION_FIXED_ARRAY_BACKING = 2,
} XrCAddressProjection;

typedef enum XrCCleanupAction {
    XR_C_CLEANUP_INVALID = 0,
    XR_C_CLEANUP_RELEASE = 1,
} XrCCleanupAction;

typedef struct XrCRecipeArgumentView {
    uint32_t semantic_value;
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
