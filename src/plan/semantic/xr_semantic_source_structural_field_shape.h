/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_source_structural_field_shape.h - Exact structural field results
 *
 * KEY CONCEPT:
 *   OBJECT_GET_F selects a field by the ordinal frozen in the operation.  A
 *   managed result is a borrowed tagged carrier only when that ordinal names
 *   the same result type in an exact source structural shape.  The judgement
 *   depends on the serialized producer, receiver layout and ownership facts;
 *   it does not depend on a module, field spelling, or consumer.
 */

#ifndef XR_SEMANTIC_SOURCE_STRUCTURAL_FIELD_SHAPE_H
#define XR_SEMANTIC_SOURCE_STRUCTURAL_FIELD_SHAPE_H

#include "xr_semantic_source_class_field_shape.h"
#include "xr_semantic_value_aggregate_shape.h"

typedef enum XrSemanticSourceStructuralFieldResultCarrier {
    XR_SEM_SOURCE_STRUCTURAL_FIELD_RESULT_NONE = 0,
    XR_SEM_SOURCE_STRUCTURAL_FIELD_RESULT_BORROWED_TAGGED,
} XrSemanticSourceStructuralFieldResultCarrier;

static inline bool xr_semantic_source_structural_field_read_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation, uint32_t *receiver_type,
    uint32_t *field_ordinal) {
    uint32_t operand_count = 0;
    uint32_t child_count = 0;
    const XrSemanticOperandRecord *operands =
        plan ? xr_semantic_plan_operands(plan, &operand_count) : NULL;
    const uint32_t *children = plan ? xr_semantic_plan_type_children(plan, &child_count) : NULL;
    XrStableId zero = {{0}};
    if (!plan || !operation || !operands || !children || operation->opcode != XI_OBJECT_GET_F ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_OBJECT_GET_F) ||
        operation->flags != xi_generated_op_default_flags(XI_OBJECT_GET_F) ||
        operation->ownership_use != xi_generated_op_own_use(XI_OBJECT_GET_F) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->evidence[0] != 0 || operation->evidence[1] != 0 || operation->evidence[2] != 0 ||
        operation->evidence[3] == 0 || operation->evidence[4] != 0 || operation->evidence[5] != 0 ||
        operation->evidence[6] != 0 || operation->evidence[7] != XR_SEMANTIC_INDEX_NONE ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE || operation->return_parameter != -1 ||
        operation->return_complete != 0 || operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->allocation_key != NULL ||
        !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE || operation->view_origin != 0 ||
        operation->view_capability != 0 || operation->view_lifetime != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_complete != 0 || operation->array_element_storage != 0 ||
        operation->array_hof_kind != XR_SEM_ARRAY_HOF_NONE ||
        operation->array_result_element_storage != 0 || operation->reserved_view[0] != 0 ||
        operation->reserved_view[1] != 0 ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;

    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, receiver->type);
    if (!type || receiver->value == XR_SEMANTIC_INDEX_NONE ||
        receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->parameter_mode != XR_PARAM_READ || receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->access != XR_CALL_ARG_PLAIN || receiver->origin != XI_PLACE_ORIGIN_NONE ||
        receiver->lifetime != XI_PLACE_LIFETIME_NONE || receiver->escape != XI_PLACE_ESCAPE_NONE ||
        receiver->flags != 0 ||
        !xr_semantic_source_structural_shape_is_exact(plan, receiver->type) ||
        type->child_begin > child_count || type->child_count > child_count - type->child_begin ||
        operation->semantic_immediate < 0 ||
        operation->semantic_immediate >= (int64_t) type->child_count)
        return false;
    uint32_t ordinal = (uint32_t) operation->semantic_immediate;
    if (children[type->child_begin + ordinal] != operation->result_type)
        return false;
    if (receiver_type)
        *receiver_type = receiver->type;
    if (field_ordinal)
        *field_ordinal = ordinal;
    return true;
}

static inline bool xr_semantic_source_structural_field_result_carrier_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint8_t *result_carrier) {
    if (!xr_semantic_source_structural_field_read_is_exact(plan, operation, NULL, NULL) ||
        !xr_semantic_managed_field_result_type_is_exact(plan, operation->result_type))
        return false;
    if (result_carrier)
        *result_carrier = XR_SEM_SOURCE_STRUCTURAL_FIELD_RESULT_BORROWED_TAGGED;
    return true;
}

#endif /* XR_SEMANTIC_SOURCE_STRUCTURAL_FIELD_SHAPE_H */
