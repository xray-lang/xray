/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_string_shape.h - Shared exactness judgement for String allocations
 *
 * KEY CONCEPT:
 *   A String is immutable and shared, so a String value carries exactly one
 *   storage fact: the outer tagged value. A concatenation is the one String
 *   producer that allocates from operands rather than from a constant, and it
 *   consumes every operand it joins. Every layer that has to answer "is this
 *   value a fresh owned String" asks this one judgement, so the target builder,
 *   the target verifier and the AOT representation oracle cannot drift into
 *   three similar-looking rules.
 */

#ifndef XR_SEMANTIC_STRING_SHAPE_H
#define XR_SEMANTIC_STRING_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_ids.h"
#include "xr_semantic_allocation_shape.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

/* An owned String type row: a reference-capable ownership root with no
 * aggregate geometry, no nullability, no borrow view and no value spelling.
 * These are the only String rows whose storage is the plain tagged value. */
static inline bool xr_semantic_owned_string_type_is_exact(const XrSemanticTypeRecord *type) {
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE |
                        XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    return type && type->kind == XR_KIND_STRING && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && (type->flags & forbidden) == 0 &&
           (type->flags & required) == required;
}

/* One judgement for a string concatenation: it joins two or more owned String
 * operands into one freshly owned String. Every operand is consumed, so the
 * result borrows nothing and the join leaves no reference-count obligation on
 * any input that this row does not already describe. An operand that is not an
 * owned String row, a result that is not one, a missing allocation identity, or
 * any deviation from the generated operation shape leaves the concatenation
 * unclaimed rather than partially proved. */
static inline bool xr_semantic_string_concat_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    if (!plan || !operation || !operands || !function ||
        operation->opcode != XI_STR_CONCAT || operation->operand_count < 2u ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_STR_CONCAT) ||
        operation->flags != xi_generated_op_default_flags(XI_STR_CONCAT) ||
        operation->ownership_use != xi_generated_op_own_use(XI_STR_CONCAT) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_STR_CONCAT) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        operation->view_complete != 0 || operation->view_source_operand != -1 ||
        operation->view_source_parameter != -1 ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        !xr_semantic_owned_string_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)) ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *piece = &operands[operation->operand_begin + i];
        if (piece->role != XR_SEM_OPERAND_VALUE || piece->parameter != -1 ||
            piece->transfer_mode != 0 || piece->ownership_action != XR_SEM_OPERAND_CONSUME ||
            piece->parameter_mode != 0 || piece->access != 0 || piece->origin != 0 ||
            piece->lifetime != 0 || piece->escape != 0 || piece->flags != 0 ||
            piece->type != operation->result_type ||
            !xr_semantic_owned_string_type_is_exact(xr_semantic_plan_type(plan, piece->type)))
            return false;
    }
    return true;
}

#endif  // XR_SEMANTIC_STRING_SHAPE_H
