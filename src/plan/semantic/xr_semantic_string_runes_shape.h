/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_string_runes_shape.h - Exact String.runes member authority
 *
 * KEY CONCEPT:
 *   The member identity is frozen by SemanticPlan. Target planning and C
 *   emission reconstruct the complete call shape from those immutable rows;
 *   neither consumer infers the member from live Xi types or selector text.
 */

#ifndef XR_SEMANTIC_STRING_RUNES_SHAPE_H
#define XR_SEMANTIC_STRING_RUNES_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

static inline bool xr_semantic_string_runes_result_type_is_exact(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    const XrSemanticTypeRecord *element =
        type && type->child_count == 1 && type->child_begin < child_count
            ? xr_semantic_plan_type(plan, children[type->child_begin])
            : NULL;
    const char expected[] =
        "type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:8:Iterator[1;"
        "type-v3:24:0:0:0:0:0:0:0:0:255:0:]";
    return type && type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 1 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->enum_layout_id == 0 && type->enum_member_count == 0 &&
           type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected) == 0 && element &&
           element->kind == XR_KIND_RUNE && element->builtin_type == XR_TID_NULL &&
           element->child_count == 0 && element->scalar_rep == XR_SCALAR_REP_NONE &&
           element->aggregate_extent == 0 && element->aggregate_align == 0 &&
           element->flags == 0;
}

static inline bool xr_semantic_string_runes_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_RUNES ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->metadata_count != 1 ||
        operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "runes") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 ||
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
    if (!receiver_type || receiver_type->kind != XR_KIND_STRING ||
        receiver_type->builtin_type != XR_TID_NULL || receiver_type->child_count != 0 ||
        receiver_type->scalar_rep != XR_SCALAR_REP_NONE ||
        receiver_type->aggregate_extent != 0 || receiver_type->aggregate_align != 0 ||
        receiver_type->source_class != XR_SEMANTIC_INDEX_NONE ||
        (receiver_type->flags &
         (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT)) !=
            (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        (receiver_type->flags &
         (XR_SEM_TYPE_NULLABLE | XR_SEM_TYPE_VALUE | XR_SEM_TYPE_BORROW_VIEW |
          XR_SEM_TYPE_AGGREGATE_EXACT)) != 0 ||
        !xr_semantic_string_runes_result_type_is_exact(plan, result_type) ||
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

#endif  // XR_SEMANTIC_STRING_RUNES_SHAPE_H
