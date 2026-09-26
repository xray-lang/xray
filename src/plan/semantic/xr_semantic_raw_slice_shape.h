/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_raw_slice_shape.h - Frozen foreign view and lifetime authority
 */

#ifndef XR_SEMANTIC_RAW_SLICE_SHAPE_H
#define XR_SEMANTIC_RAW_SLICE_SHAPE_H

#include "xr_semantic_array_element_storage_shape.h"
#include "xr_semantic_native_module_shape.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"

static inline bool xr_semantic_raw_slice_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    if (!plan || !operation || operation->opcode != XI_SLICE_FROM_PTR)
        return false;
    uint32_t operand_count = 0, child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticTypeRecord *element = xr_semantic_plan_type(plan, operation->view_element_type);
    bool mutable_view = (operation->semantic_immediate & XI_SLICE_FROM_PTR_AUX_MUTABLE) != 0;
    uint8_t view_flags = XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_BORROW_VIEW |
                         (mutable_view ? 0 : XR_SEM_TYPE_CONST);
    if (!operands || !children || !result || !element || operation->operand_count != 3 ||
        operation->operand_begin > operand_count || operand_count - operation->operand_begin < 3 ||
        result->kind != XR_KIND_SLICE || result->child_count != 1 ||
        result->child_begin >= child_count || result->flags != view_flags ||
        children[result->child_begin] != operation->view_element_type ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->effects != xi_generated_op_effects(XI_SLICE_FROM_PTR) ||
        operation->flags != (XI_FLAG_READS_MEM | (mutable_view ? XI_FLAG_WRITES_MEM : 0)) ||
        operation->ownership_use != XI_GEN_OWN_USE_BORROW ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->view_source_operand != 2 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_FOREIGN ||
        operation->view_capability != (mutable_view ? 2 : 1) ||
        operation->view_lifetime != 1 || operation->view_complete != 1 ||
        operation->reserved_view[0] || operation->reserved_view[1])
        return false;
    const XrSemanticOperandRecord *args = operands + operation->operand_begin;
    for (unsigned i = 0; i < 3; ++i)
        if (args[i].role != XR_SEM_OPERAND_VALUE || args[i].parameter != -1 ||
            args[i].ownership_action != XR_SEM_OPERAND_BORROW)
            return false;
    const XrSemanticTypeRecord *pointer = xr_semantic_plan_type(plan, args[0].type);
    const XrSemanticTypeRecord *count = xr_semantic_plan_type(plan, args[1].type);
    unsigned pointer_mutable = 0;
    if (!xr_semantic_raw_pointer_type_mutability(pointer, &pointer_mutable) ||
        (mutable_view && !pointer_mutable) || !count || count->kind != XR_KIND_INT ||
        count->flags != 0 || args[2].value != operation->view_source_value)
        return false;
    uint64_t packed = (uint64_t) operation->semantic_immediate;
    uint32_t size = (uint32_t) ((packed >> 8) & UINT64_C(0xffff));
    uint32_t alignment = (uint32_t) ((packed >> 32) & UINT64_C(0xffff));
    if ((packed >> 49) != 0 || size == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0)
        return false;
    uint8_t storage = xr_semantic_array_element_storage(element);
    if (storage != XR_ELEM_ANY &&
        ((uint8_t) packed != storage || size != XR_ELEM_SIZES[storage] ||
         alignment != XR_ELEM_SIZES[storage] ||
         xr_tid_to_elem_type((uint8_t) (packed >> 24)) != storage))
        return false;
    return true;
}

#endif  // XR_SEMANTIC_RAW_SLICE_SHAPE_H
