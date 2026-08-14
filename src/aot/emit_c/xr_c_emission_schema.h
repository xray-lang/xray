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
    XR_C_VALUE_MATERIALIZATION_COUNT,
} XrCValueMaterializationRecipe;

typedef enum XrCRecipeArgumentKind {
    XR_C_RECIPE_ARGUMENT_INVALID = 0,
    XR_C_RECIPE_ARGUMENT_STRING_VALUE = 1,
    XR_C_RECIPE_ARGUMENT_ENUM_PAYLOAD = 2,
} XrCRecipeArgumentKind;

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
    const char *c_type;
    const char *literal_bytes;
    const char *recipe_symbol;
    const char *recipe_type_name;
    const char *recipe_member_name;
    const XrCRecipeArgumentView *recipe_arguments;
} XrCValueEmissionView;

#endif  // XR_C_EMISSION_SCHEMA_H
