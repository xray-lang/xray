/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_range_slice_shape.h - Shared exactness judgement for the borrowed
 * view a range slice produces. String slicing is a method call and has its own
 * judgement; this one covers `container[start:end]`, whose result borrows part
 * of a container it did not allocate.
 */

#ifndef XR_SEMANTIC_RANGE_SLICE_SHAPE_H
#define XR_SEMANTIC_RANGE_SLICE_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"

/* A borrow view over one exact scalar element. The view itself is a pointer and
 * a length whatever the element is, but the element still has to be exact: its
 * stride is what turns the length into bytes, and a reference-carrying or
 * aggregate element would put a reference-count obligation behind the borrow
 * that no family here discharges. */
static inline bool xr_semantic_slice_view_type_is_exact(const XrSemanticPlan *plan,
                                                        uint32_t type_index,
                                                        uint32_t *out_element_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, type_index);
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const uint8_t required = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW;
    const uint8_t allowed = (uint8_t) (required | XR_SEM_TYPE_CONST);
    if (!plan || !type || !children || type->kind != XR_KIND_SLICE ||
        type->builtin_type != XR_TID_NULL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 || type->child_count != 1 ||
        type->child_begin >= child_count || (type->flags & required) != required ||
        (type->flags & (uint8_t) ~allowed) != 0)
        return false;
    uint32_t element_type = children[type->child_begin];
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, element_type);
    if (!element || element->builtin_type != XR_TID_NULL || element->flags != 0 ||
        element->child_count != 0 || element->aggregate_extent != 0 ||
        element->aggregate_align != 0 || element->source_class != XR_SEMANTIC_INDEX_NONE)
        return false;
    bool element_is_exact_scalar = false;
    switch (element->kind) {
        case XR_KIND_INT:
            element_is_exact_scalar =
                element->scalar_rep == XR_NATIVE_I8 || element->scalar_rep == XR_NATIVE_U8 ||
                element->scalar_rep == XR_NATIVE_I16 || element->scalar_rep == XR_NATIVE_U16 ||
                element->scalar_rep == XR_NATIVE_I32 || element->scalar_rep == XR_NATIVE_U32 ||
                element->scalar_rep == XR_NATIVE_I64 || element->scalar_rep == XR_NATIVE_U64;
            break;
        case XR_KIND_FLOAT:
            element_is_exact_scalar =
                element->scalar_rep == XR_NATIVE_F32 || element->scalar_rep == XR_NATIVE_F64;
            break;
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            element_is_exact_scalar = element->scalar_rep == XR_SCALAR_REP_NONE;
            break;
        default:
            element_is_exact_scalar = false;
            break;
    }
    if (!element_is_exact_scalar)
        return false;
    if (out_element_type)
        *out_element_type = element_type;
    return true;
}

/* One judgement for `container[start:end]`. The result borrows the container's
 * own elements, so it allocates nothing and owns nothing, and its two bounds are
 * plain native integers. The container operand is left to the storage family
 * that already bound it: this judgement proves the shape of the view, not which
 * of the three container carriers it was taken from.
 *
 * The builder publishes the view's storage from this judgement, the independent
 * verifier rebuilds from it, and the AOT representation pass re-proves the same
 * relation, so no layer can widen the family on its own. */
static inline bool xr_semantic_range_slice_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation,
                                                    uint32_t *out_element_type) {
    XrStableId zero = {{0}};
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    if (!plan || !operation || !operands || !function || operation->opcode != XI_SLICE ||
        operation->operand_count != 3u || operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE || operation->metadata_count != 0 ||
        operation->semantic_immediate != 0 || operation->auxiliary_kind != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_SLICE) ||
        operation->flags != xi_generated_op_default_flags(XI_SLICE) ||
        operation->ownership_use != xi_generated_op_own_use(XI_SLICE) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_SLICE) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->return_parameter != -1 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count ||
        !xr_semantic_slice_view_type_is_exact(plan, operation->result_type, out_element_type))
        return false;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 ||
        source->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    for (uint16_t i = 1; i < 3u; i++) {
        const XrSemanticOperandRecord *bound = &operands[operation->operand_begin + i];
        const XrSemanticTypeRecord *bound_type = xr_semantic_plan_type(plan, bound->type);
        if (bound->role != XR_SEM_OPERAND_VALUE || bound->parameter != -1 ||
            bound->ownership_action != XR_SEM_OPERAND_BORROW || !bound_type ||
            bound_type->kind != XR_KIND_INT || bound_type->scalar_rep != XR_NATIVE_I64 ||
            bound_type->flags != 0 || bound_type->child_count != 0 ||
            bound_type->builtin_type != XR_TID_NULL)
            return false;
    }
    return true;
}

#endif  // XR_SEMANTIC_RANGE_SLICE_SHAPE_H
