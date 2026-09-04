/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_string_slice_shape.h - Exact String.slice(start, end) authority
 *
 * KEY CONCEPT:
 *   SemanticPlan freezes the three-operand member identity and both ordered
 *   rune bounds. Later target and C consumers never rediscover this builtin
 *   from selector text, live Xi types, or argument count.
 */

#ifndef XR_SEMANTIC_STRING_SLICE_SHAPE_H
#define XR_SEMANTIC_STRING_SLICE_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_string_shape.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

static inline bool xr_semantic_string_slice_string_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    const char expected[] = "type-v3:2:0:0:0:0:0:0:0:0:255:0:";
    return type && type->kind == XR_KIND_STRING && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->child_count == 0 && type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected) == 0;
}

static inline bool xr_semantic_string_slice_i64_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    const char expected[] = "type-v3:0:0:0:0:0:0:0:0:0:0:0:";
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->child_count == 0 && type->scalar_rep == XR_NATIVE_I64 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == 0 && type->canonical_key && strcmp(type->canonical_key, expected) == 0;
}

static inline bool xr_semantic_string_slice_operand_is_exact(const XrSemanticOperandRecord *operand,
                                                             uint8_t role, int32_t parameter,
                                                             uint8_t ownership_action) {
    return operand && operand->role == role && operand->parameter == parameter &&
           operand->transfer_mode == XR_TRANSFER_SHARE &&
           operand->ownership_action == ownership_action &&
           operand->parameter_mode == XR_PARAM_READ && operand->access == XR_CALL_ARG_PLAIN &&
           operand->origin == 0 && operand->lifetime == 0 && operand->escape == 0 &&
           operand->flags == XR_SEM_OPERAND_CALL_CONTRACT;
}

static inline bool
xr_semantic_string_slice_receiver_has_prior_identity(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *slice,
                                                     const XrSemanticOperandRecord *receiver) {
    if (!plan || !slice || !receiver)
        return false;
    uint32_t matches = 0;
    size_t parameter_count = xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < (uint32_t) parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, i);
        if (!parameter || parameter->value != receiver->value)
            continue;
        /* Requiredness decides whether a caller may omit this parameter; it
         * does not change the identity or carrier of the value once the body
         * reads it. Reuse the generic String-parameter judgement so required
         * and defaulted parameters obey one ownership/storage contract. */
        if (parameter->function != slice->function || parameter->type != receiver->type ||
            !xr_semantic_direct_local_string_value_parameter_is_exact(plan, parameter, NULL) ||
            !parameter->canonical_key)
            return false;
        matches++;
    }
    if (matches == 1)
        return true;
    if (matches != 0)
        return false;
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < (uint32_t) operation_count; i++) {
        const XrSemanticOperationRecord *source = xr_semantic_plan_operation(plan, i);
        if (!source || source->result_value != receiver->value)
            continue;
        if (source->function != slice->function || source->result_type != receiver->type ||
            source->opcode != XI_CONST || source->operand_count != 0 ||
            source->allocation_key != NULL ||
            source->constant >= xr_semantic_plan_constant_count(plan) ||
            source->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
            source->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
            source->return_complete != 1)
            return false;
        XrStableId zero = {{0}};
        const XrSemanticConstantRecord *constant =
            xr_semantic_plan_constant(plan, source->constant);
        if (!xr_stable_id_equal(source->allocation_id, zero) || !constant ||
            constant->kind != XR_SEM_CONST_STRING || !constant->string ||
            constant->type != receiver->type)
            return false;
        matches++;
    }
    return matches == 1;
}

static inline bool xr_semantic_string_slice_range_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *start_value, uint32_t *end_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_STRING_SLICE_RANGE ||
        operation->opcode != XI_CALL_METHOD || operation->semantic_immediate <= 0 ||
        (operation->semantic_immediate & 1) != 0 || operation->operand_count != 3 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        strcmp(metadata[operation->metadata_begin], "slice") != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        /* Tail position is a fact about where the call sits, not about whether
         * it is a slice. Requiring it made `print(s.slice(a, b))` inexpressible
         * on the AOT side while `return s.slice(a, b)` was fine -- the same
         * operation, refused for its surroundings. Both spellings are accepted;
         * every other field still has to match exactly. */
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
    const XrSemanticOperandRecord *start = receiver + 1;
    const XrSemanticOperandRecord *end = receiver + 2;
    const XrSemanticTypeRecord *result_type = xr_semantic_plan_type(plan, operation->result_type);
    const XrSemanticTypeRecord *receiver_type = xr_semantic_plan_type(plan, receiver->type);
    const XrSemanticTypeRecord *start_type = xr_semantic_plan_type(plan, start->type);
    const XrSemanticTypeRecord *end_type = xr_semantic_plan_type(plan, end->type);
    if (operation->result_type != receiver->type || start->type != end->type ||
        !xr_semantic_string_slice_string_type_is_exact(result_type) ||
        !xr_semantic_string_slice_string_type_is_exact(receiver_type) ||
        !xr_semantic_string_slice_i64_type_is_exact(start_type) ||
        !xr_semantic_string_slice_i64_type_is_exact(end_type) ||
        !xr_semantic_string_slice_receiver_has_prior_identity(plan, operation, receiver) ||
        !xr_semantic_string_slice_operand_is_exact(receiver, XR_SEM_OPERAND_RECEIVER, -1,
                                                   XR_SEM_OPERAND_BORROW) ||
        !xr_semantic_string_slice_operand_is_exact(start, XR_SEM_OPERAND_ARGUMENT, 0,
                                                   XR_SEM_OPERAND_CONSUME) ||
        !xr_semantic_string_slice_operand_is_exact(end, XR_SEM_OPERAND_ARGUMENT, 1,
                                                   XR_SEM_OPERAND_CONSUME))
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    if (start_value)
        *start_value = start->value;
    if (end_value)
        *end_value = end->value;
    return true;
}

#endif  // XR_SEMANTIC_STRING_SLICE_SHAPE_H
