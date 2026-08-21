/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_rune_to_string_shape.h - Exact iterator-rune to string authority
 */

#ifndef XR_SEMANTIC_RUNE_TO_STRING_SHAPE_H
#define XR_SEMANTIC_RUNE_TO_STRING_SHAPE_H

#include "xr_semantic_rune_to_uint32_shape.h"

/* `r.toString()` where the rune came from a `String.runes()` iterator: the
 * one-rune string. Same receiver requirement as toUInt32 next door -- the rune
 * has to be one this plan can trace back to an iterator, not any rune at all,
 * because that is what makes the conversion's operand exact.
 */
static inline bool xr_semantic_rune_to_string_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation,
                                                       uint32_t *receiver_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_RUNE_TO_STRING ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "toString") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER)
        return false;
    if (!xr_semantic_rune_to_uint32_receiver_is_exact(plan, operation, receiver->value))
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    return true;
}

#endif  // XR_SEMANTIC_RUNE_TO_STRING_SHAPE_H
