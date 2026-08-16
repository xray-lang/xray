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
 *   storage fact: the outer tagged value, whoever holds it. A concatenation is
 *   the one String producer that allocates from operands rather than from a
 *   constant, and it consumes every operand it joins. Passing a String by value
 *   allocates nothing at all: the callee borrows the caller's allocation in that
 *   same tagged carrier. Every layer that has to answer "which String shape is
 *   this" asks these judgements, so the target builder, the target verifier and
 *   the AOT representation oracle cannot drift into three similar-looking rules.
 */

#ifndef XR_SEMANTIC_STRING_SHAPE_H
#define XR_SEMANTIC_STRING_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_ids.h"
#include "xr_semantic_allocation_shape.h"
#include "xr_semantic_shared_read_shape.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

/* A String type row whose storage is the plain tagged value: a reference-capable
 * ownership root with no aggregate geometry, no nullability, no borrow view and
 * no value spelling. The judgement is about the row, not about who holds the
 * value -- a freshly concatenated String and a String a callee borrowed from its
 * caller share one row -- so it says "tagged" rather than naming an ownership
 * the row cannot know. */
static inline bool xr_semantic_tagged_string_type_is_exact(const XrSemanticTypeRecord *type) {
    uint8_t forbidden = XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
                        XR_SEM_TYPE_AGGREGATE_EXACT;
    uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT;
    return type && type->kind == XR_KIND_STRING && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && (type->flags & forbidden) == 0 &&
           (type->flags & required) == required;
}

/* A String parameter handed over by value.
 *
 * A String is immutable and shared, so passing one by value borrows: the callee
 * reads the caller's allocation for the extent of the call and releases
 * nothing. Its one storage fact is the outer tagged value, the same carrier a
 * String literal, a concatenation result and a direct-local String result all
 * select, so the parameter needs no place of its own and no addressability. A
 * `ref` String parameter is a different boundary -- it names the caller's cell
 * and may rebind it -- and stays unclaimed here.
 *
 * TargetPlan construction, TargetPlan verification and representation
 * refinement all ask this one judgement, so a parameter one layer binds cannot
 * be one another layer refuses. */
static inline bool xr_semantic_direct_local_string_value_parameter_is_exact(
    const XrSemanticPlan *plan, const XrSemanticParameterRecord *parameter) {
    return plan && parameter && parameter->function < xr_semantic_plan_function_count(plan) &&
           parameter->value != XR_SEMANTIC_INDEX_NONE && parameter->mode == XR_PARAM_READ &&
           parameter->ownership == XI_OWN_BORROWED &&
           parameter->transfer_mode == XR_TRANSFER_SHARE &&
           (parameter->flags & ~XR_SEM_PARAMETER_REQUIRED) == 0 && parameter->reserved == 0 &&
           xr_semantic_tagged_string_type_is_exact(xr_semantic_plan_type(plan, parameter->type));
}

/* A String read back out of the shared cell it was bound to.
 *
 * A String is immutable and shared, so reading the cell borrows the one
 * allocation it holds, in the same tagged carrier a literal, a concatenation
 * and a direct-local result all select. This is the shape every program that
 * names a string after binding it has, so it is a String value in its own
 * right, not something a call boundary confers -- the boundary is only one of
 * the places that has to recognise it.
 *
 * The result type is proved here rather than left to whoever asks: a judgement
 * that admitted every shared read and relied on its caller to have narrowed the
 * type would claim Array and nested-container reads the moment it was applied
 * anywhere else. The value's definition must also be this one operation, since
 * the storage bound to it has to describe the value's whole life.
 *
 * TargetPlan construction, TargetPlan verification and representation
 * refinement all ask this one judgement, so a read one layer binds cannot be
 * one another layer refuses. */
static inline bool
xr_semantic_tagged_string_shared_read_is_exact(const XrSemanticPlan *plan,
                                               const XrSemanticOperationRecord *operation) {
    return plan && operation && xr_semantic_shared_read_operation_is_exact(operation) &&
           xr_semantic_tagged_string_type_is_exact(
               xr_semantic_plan_type(plan, operation->result_type)) &&
           xr_semantic_unique_value_definition(plan, operation->result_value) == operation;
}

/* One exact native unsigned display source. It owns no reference, aggregate,
 * nullable encoding, nominal identity, or child geometry; those cases require
 * different display recipes and remain unclaimed. */
static inline bool
xr_semantic_string_concat_direct_u64_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->scalar_rep == XR_NATIVE_U64 && type->flags == 0 && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) && !type->source_enum_key &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 && type->enum_flags == 0 &&
           type->reserved_enum == 0;
}

/* One judgement for a string concatenation: it joins two or more exact display
 * operands into one freshly owned String. A String operand is consumed in its
 * owned tagged carrier. An exact u64 operand is consumed as a logical display
 * value whose native source remains independently frozen by TargetPlan and the
 * C-emission recipe. Every other display shape stays unclaimed. */
static inline bool xr_semantic_string_concat_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    if (!plan || !operation || !operands || !function || operation->opcode != XI_STR_CONCAT ||
        operation->operand_count < 2u || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->metadata_count != 0 ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != 0 ||
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
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        !xr_semantic_allocation_identity_is_canonical(operation) ||
        !xr_semantic_tagged_string_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)) ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *piece = &operands[operation->operand_begin + i];
        const XrSemanticTypeRecord *piece_type = xr_semantic_plan_type(plan, piece->type);
        if (piece->role != XR_SEM_OPERAND_VALUE || piece->parameter != -1 ||
            piece->transfer_mode != 0 || piece->ownership_action != XR_SEM_OPERAND_CONSUME ||
            piece->parameter_mode != 0 || piece->access != 0 || piece->origin != 0 ||
            piece->lifetime != 0 || piece->escape != 0 || piece->flags != 0 ||
            !((piece->type == operation->result_type &&
               xr_semantic_tagged_string_type_is_exact(piece_type)) ||
              xr_semantic_string_concat_direct_u64_type_is_exact(piece_type)))
            return false;
    }
    return true;
}

#endif  // XR_SEMANTIC_STRING_SHAPE_H
