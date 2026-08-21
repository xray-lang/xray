/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_iterator_rune_nth_shape.h - Exact String.runes iterator nth authority
 */

#ifndef XR_SEMANTIC_ITERATOR_RUNE_NTH_SHAPE_H
#define XR_SEMANTIC_ITERATOR_RUNE_NTH_SHAPE_H

#include "xr_semantic_iterator_rune_next_shape.h"

/* `s.runes().nth(i)`: the rune at an index, over the same receiver `next`
 * walks. It differs from `next` in taking an index and in leaving the iterator
 * where it was, so it is a projection of the string rather than a step through
 * it -- which is why an index argument is allowed here and nowhere else in
 * this family.
 *
 * The receiver judgement is shared with `next` rather than restated: both
 * require the receiver to be exactly a `String.runes()` result defined in the
 * same function.
 */
static inline bool
xr_semantic_iterator_rune_nth_is_exact(const XrSemanticPlan *plan,
                                       const XrSemanticOperationRecord *operation,
                                       uint32_t *receiver_value, uint32_t *index_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ITERATOR_RUNE_NTH ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 2 ||
        operation->operand_begin + 1u >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "nth") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = &operands[operation->operand_begin + 1u];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || index->role != XR_SEM_OPERAND_ARGUMENT)
        return false;
    if (!xr_semantic_iterator_rune_next_receiver_is_exact(plan, operation, receiver->value))
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (index_value)
        *index_value = index->value;
    return true;
}

/* A rune produced by a `String.runes()` iterator, by either spelling. The Xi
 * layer has the same judgement under the same name; both exist because each
 * layer sees a different record, and letting only one of them accept `nth` is
 * exactly the drift this line keeps removing. */
static inline bool
xr_semantic_iterator_rune_source_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation) {
    return xr_semantic_iterator_rune_next_is_exact(plan, operation, NULL) ||
           xr_semantic_iterator_rune_nth_is_exact(plan, operation, NULL, NULL);
}

#endif  // XR_SEMANTIC_ITERATOR_RUNE_NTH_SHAPE_H
