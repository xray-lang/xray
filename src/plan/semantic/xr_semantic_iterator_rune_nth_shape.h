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
        operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate != (int64_t) XI_METHOD_SYMBOL_NTH << 1 ||
        operation->operand_count != 2 || operation->operand_begin + 1u >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "nth") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        (operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) &&
         operation->flags != (xi_generated_op_default_flags(XI_CALL_METHOD) | XI_FLAG_TAIL)) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = &operands[operation->operand_begin + 1u];
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *index_type = xr_semantic_plan_type(plan, index->type);
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    XrStableId zero = {{0}};
    const char expected_int[] = "type-v3:0:0:0:0:0:0:0:0:0:0:0:";
    const char expected_rune[] = "type-v3:24:0:0:0:0:0:0:0:0:255:0:";
    if (!xr_semantic_string_runes_result_type_is_exact(plan, receiver_type) || !index_type ||
        index_type->kind != XR_KIND_INT || index_type->builtin_type != XR_TID_NULL ||
        index_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(index_type->source_class_identity, zero) ||
        !xr_stable_id_equal(index_type->source_enum_identity, zero) ||
        index_type->source_enum_key || index_type->child_count != 0 ||
        index_type->scalar_rep != XR_NATIVE_I64 || index_type->aggregate_extent != 0 ||
        index_type->aggregate_align != 0 || index_type->enum_layout_id != 0 ||
        index_type->enum_member_count != 0 || index_type->enum_flags != 0 ||
        index_type->reserved_enum != 0 || index_type->flags != 0 || !index_type->canonical_key ||
        strcmp(index_type->canonical_key, expected_int) != 0 || !result_type ||
        result_type->kind != XR_KIND_RUNE || result_type->builtin_type != XR_TID_NULL ||
        result_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(result_type->source_class_identity, zero) ||
        !xr_stable_id_equal(result_type->source_enum_identity, zero) ||
        result_type->source_enum_key || result_type->child_count != 0 ||
        result_type->scalar_rep != XR_SCALAR_REP_NONE || result_type->aggregate_extent != 0 ||
        result_type->aggregate_align != 0 || result_type->enum_layout_id != 0 ||
        result_type->enum_member_count != 0 || result_type->enum_flags != 0 ||
        result_type->reserved_enum != 0 || result_type->flags != 0 || !result_type->canonical_key ||
        strcmp(result_type->canonical_key, expected_rune) != 0 ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->parameter_mode != 0 ||
        receiver->access != 0 || receiver->origin != 0 || receiver->lifetime != 0 ||
        receiver->escape != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        index->role != XR_SEM_OPERAND_ARGUMENT || index->parameter != 0 ||
        index->transfer_mode != XR_TRANSFER_SHARE ||
        index->ownership_action != XR_SEM_OPERAND_CONSUME ||
        index->parameter_mode != XR_PARAM_READ || index->access != XR_CALL_ARG_PLAIN ||
        index->origin != 0 || index->lifetime != 0 || index->escape != 0 ||
        index->flags != XR_SEM_OPERAND_CALL_CONTRACT)
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
