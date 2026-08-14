/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_iterator_rune_has_next_shape.h - Exact Iterator<rune>.hasNext authority
 */

#ifndef XR_SEMANTIC_ITERATOR_RUNE_HAS_NEXT_SHAPE_H
#define XR_SEMANTIC_ITERATOR_RUNE_HAS_NEXT_SHAPE_H

#include "xr_semantic_string_runes_shape.h"

static inline bool xr_semantic_iterator_rune_has_next_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_ITERATOR_RUNE_HAS_NEXT ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "hasNext") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE || operation->return_complete != 0 ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *receiver_type =
        xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *result_type =
        xr_semantic_plan_type(plan, operation->result_type);
    XrStableId zero = {{0}};
    const char expected_bool[] = "type-v3:3:0:0:0:0:0:0:0:0:255:0:";
    if (!xr_semantic_string_runes_result_type_is_exact(plan, receiver_type) || !result_type ||
        result_type->kind != XR_KIND_BOOL || result_type->builtin_type != XR_TID_NULL ||
        result_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(result_type->source_class_identity, zero) ||
        !xr_stable_id_equal(result_type->source_enum_identity, zero) ||
        result_type->source_enum_key || result_type->child_count != 0 ||
        result_type->scalar_rep != XR_SCALAR_REP_NONE || result_type->aggregate_extent != 0 ||
        result_type->aggregate_align != 0 || result_type->enum_layout_id != 0 ||
        result_type->enum_member_count != 0 || result_type->enum_flags != 0 ||
        result_type->reserved_enum != 0 || result_type->flags != 0 ||
        !result_type->canonical_key || strcmp(result_type->canonical_key, expected_bool) != 0 ||
        receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != 0 || receiver->access != 0 || receiver->origin != 0 ||
        receiver->lifetime != 0 || receiver->escape != 0 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    return true;
}

#endif  // XR_SEMANTIC_ITERATOR_RUNE_HAS_NEXT_SHAPE_H
