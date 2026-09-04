/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_array_index_shape.h - Exact Array element-read authority
 *
 * KEY CONCEPT:
 *   INDEX_GET is shared by several language constructs, so its opcode does not
 *   prove Array storage. An Array element read is the narrower relation among
 *   one exact Array<T> receiver, one exact i64 index, and one borrowed T result.
 *   SemanticPlan verification freezes that relation here. Target construction
 *   and independent verification may then select a representation from T
 *   without guessing it from an opcode or from a use site.
 */

#ifndef XR_SEMANTIC_ARRAY_INDEX_SHAPE_H
#define XR_SEMANTIC_ARRAY_INDEX_SHAPE_H

#include "xr_semantic_array_member_shape.h"
#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"

static inline bool xr_semantic_array_index_read_is_exact(const XrSemanticPlan *plan,
                                                         const XrSemanticOperationRecord *operation,
                                                         uint32_t *array_value,
                                                         uint32_t *index_value) {
    uint32_t operand_count = 0;
    uint32_t child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    XrStableId zero = {{0}};
    if (array_value)
        *array_value = XR_SEMANTIC_INDEX_NONE;
    if (index_value)
        *index_value = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !operation || !operands || !children || operation->opcode != XI_INDEX_GET ||
        operation->operand_count != 2 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->semantic_immediate != 0 || operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->effects != xi_generated_op_effects(XI_INDEX_GET) ||
        operation->flags != xi_generated_op_default_flags(XI_INDEX_GET) ||
        operation->ownership_use != xi_generated_op_own_use(XI_INDEX_GET) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_INDEX_GET) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->return_complete != 0 || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->array_element_storage != 0 || operation->reserved_view[0] != 0 ||
        operation->reserved_view[1] != 0 || operation->array_hof_kind != 0 ||
        operation->array_result_element_storage != 0 || operation->allocation_key != NULL ||
        !xr_stable_id_equal(operation->allocation_id, zero))
        return false;
    for (size_t i = 0; i + 1u < sizeof(operation->evidence) / sizeof(operation->evidence[0]); i++)
        if (operation->evidence[i] != 0)
            return false;
    if (operation->evidence[7] != XR_SEMANTIC_INDEX_NONE)
        return false;

    const XrSemanticOperandRecord *array = &operands[operation->operand_begin];
    const XrSemanticOperandRecord *index = array + 1;
    const XrSemanticTypeRecord *array_type = xr_semantic_plan_type(plan, array->type);
    const XrSemanticTypeRecord *index_type = xr_semantic_plan_type(plan, index->type);
    if (!xr_semantic_array_type_row_is_exact(array_type) ||
        array_type->child_begin >= child_count ||
        children[array_type->child_begin] != operation->result_type ||
        !xr_semantic_array_member_i64_type_is_exact(index_type))
        return false;
    for (uint16_t ordinal = 0; ordinal < 2; ordinal++) {
        const XrSemanticOperandRecord *operand = array + ordinal;
        if (operand->value == XR_SEMANTIC_INDEX_NONE || operand->role != XR_SEM_OPERAND_VALUE ||
            operand->parameter != -1 || operand->transfer_mode != XR_TRANSFER_SHARE ||
            operand->ownership_action != XR_SEM_OPERAND_BORROW ||
            operand->parameter_mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN ||
            operand->origin != XI_PLACE_ORIGIN_NONE ||
            operand->lifetime != XI_PLACE_LIFETIME_NONE ||
            operand->escape != XI_PLACE_ESCAPE_NONE || operand->flags != 0)
            return false;
    }
    if (xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;
    if (array_value)
        *array_value = array->value;
    if (index_value)
        *index_value = index->value;
    return true;
}

/* A managed element uses the tagged Array lane only when its lifecycle is
 * already closed by the Array member contract. This is the same type-exact
 * roster used when an Array stores a reference: String, a frozen source-class
 * instance, or another exact Array. INDEX_GET borrows such a lane; it never
 * creates a new owner. */
static inline bool
xr_semantic_array_index_tagged_read_is_exact(const XrSemanticPlan *plan,
                                             const XrSemanticOperationRecord *operation,
                                             uint32_t *array_value, uint32_t *index_value) {
    return xr_semantic_array_index_read_is_exact(plan, operation, array_value, index_value) &&
           xr_semantic_array_member_owned_reference_type_is_exact(
               plan, xr_semantic_plan_type(plan, operation->result_type));
}

#endif /* XR_SEMANTIC_ARRAY_INDEX_SHAPE_H */
