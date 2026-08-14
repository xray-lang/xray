/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_task_shape.h - Shared exactness judgements for Task values
 */

#ifndef XR_SEMANTIC_TASK_SHAPE_H
#define XR_SEMANTIC_TASK_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static inline bool xr_semantic_shape_stable_id_is_zero(XrStableId id) {
    for (uint32_t i = 0; i < XR_STABLE_ID_BYTES; i++)
        if (id.bytes[i] != 0)
            return false;
    return true;
}

/* Rebuild the builtin nominal identity from the frozen type key. This is kept
 * here instead of in a target or AOT layer so every consumer agrees on what a
 * Task<T> type is before it assigns storage to a value of that type. */
static inline bool xr_semantic_task_type_is_exact(const XrSemanticPlan *plan,
                                                  uint32_t semantic_type) {
    const XrSemanticTypeRecord *type = xr_semantic_plan_type(plan, semantic_type);
    if (!plan || !type || !type->canonical_key || type->kind != XR_KIND_INSTANCE ||
        type->builtin_type != XR_TID_COROUTINE || type->source_class != XR_SEMANTIC_INDEX_NONE ||
        type->child_count != 1 || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->source_enum_key || !xr_semantic_shape_stable_id_is_zero(type->source_enum_identity) ||
        type->enum_layout_id != 0 || type->enum_member_count != 0 || type->enum_flags != 0 ||
        type->reserved_enum != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!children || type->child_begin >= child_count ||
        children[type->child_begin] >= xr_semantic_plan_type_count(plan))
        return false;
    unsigned key_kind = 0;
    unsigned key_semantic_type = 0;
    unsigned key_builtin_type = 0;
    unsigned nullable = 0;
    unsigned is_const = 0;
    unsigned is_value = 0;
    unsigned is_literal = 0;
    unsigned cycle_candidate = 0;
    unsigned pointer_mutable = 0;
    unsigned scalar_rep = 0;
    size_t alias_length = 0;
    int consumed = 0;
    if (sscanf(type->canonical_key,
               "type-v3:%u:%u:%u:%u:%u:%u:%u:%u:%u:%u:%zu:%n",
               &key_kind, &key_semantic_type, &key_builtin_type, &nullable,
               &is_const, &is_value, &is_literal, &cycle_candidate,
               &pointer_mutable, &scalar_rep, &alias_length, &consumed) != 11 ||
        consumed <= 0 || key_kind != XR_KIND_INSTANCE || key_semantic_type != 0 ||
        key_builtin_type != XR_TID_COROUTINE || nullable != 0 || is_const != 0 ||
        is_value != 0 || is_literal != 0 || cycle_candidate != 0 ||
        pointer_mutable != 0 || scalar_rep != XR_SCALAR_REP_NONE)
        return false;
    size_t key_length = strlen(type->canonical_key);
    if (alias_length > key_length - (size_t) consumed)
        return false;
    const char *named = type->canonical_key + consumed + alias_length;
    static const char prefix[] = ";named:4:Task[1";
    return strncmp(named, prefix, sizeof(prefix) - 1u) == 0 &&
           key_length > (size_t) consumed + alias_length + sizeof(prefix) - 1u &&
           type->canonical_key[key_length - 1u] == ']';
}

/* The target-independent half of a direct-local GO result proof. The caller
 * supplies the independently rebuilt callee identity; this judgement then
 * proves that the operation produces exactly one borrowed runtime-owned
 * Task<T> handle and returns the frozen callee operand identity. */
static inline bool xr_semantic_direct_local_go_task_result_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    bool callee_identity_exact, uint32_t *callee_value) {
    if (callee_value)
        *callee_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    const uint64_t allowed_aux = (uint64_t) XI_GO_AUX_LINK_MASK |
                                 (uint64_t) XI_GO_AUX_ONE_SHOT_AWAIT |
                                 (uint64_t) XI_GO_AUX_DEFER_BATCH |
                                 (uint64_t) XI_GO_AUX_RESULT_COPY_SHARED;
    if (!plan || !operation || !operands || !function || !callee_identity_exact ||
        operation->opcode != XI_GO || operation->operand_count == 0 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count ||
        !xr_semantic_task_type_is_exact(plan, operation->result_type) ||
        operation->metadata_count != 0 || operation->auxiliary_kind != 0 ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->allocation_key ||
        !xr_semantic_shape_stable_id_is_zero(operation->allocation_id) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->semantic_immediate < 0 ||
        ((uint64_t) operation->semantic_immediate & ~allowed_aux) != 0 ||
        operation->effects != xi_generated_op_effects(XI_GO) ||
        operation->flags != xi_generated_op_default_flags(XI_GO) ||
        operation->ownership_use != xi_generated_op_own_use(XI_GO) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_GO) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_NONE ||
        operation->return_parameter != -1 || operation->return_complete != 0 ||
        operation->view_complete != 0 || operation->view_source_operand != -1 ||
        operation->view_source_parameter != -1)
        return false;
    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_VALUE || callee->parameter != -1 ||
        callee->transfer_mode != XR_TRANSFER_SHARE ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee->parameter_mode != XR_PARAM_READ || callee->access != XR_CALL_ARG_PLAIN ||
        callee->origin != XI_PLACE_ORIGIN_NONE || callee->lifetime != XI_PLACE_LIFETIME_NONE ||
        callee->escape != XI_PLACE_ESCAPE_NONE || callee->flags != 0)
        return false;
    if (callee_value)
        *callee_value = callee->value;
    return true;
}

/* A plain Task<T> carrier is consumed or borrowed by AWAIT, but never adapted.
 * Aggregate await forms have an Array receiver and deliberately do not match. */
static inline bool xr_semantic_await_task_operand_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint16_t operand_index, uint32_t source_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !operation || !operands || operation->opcode != XI_AWAIT ||
        operand_index != 0 || operation->operand_count == 0 ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin)
        return false;
    const XrSemanticOperandRecord *task = &operands[operation->operand_begin];
    return task->value == source_value &&
           xr_semantic_task_type_is_exact(plan, task->type) &&
           task->role == XR_SEM_OPERAND_VALUE && task->parameter == -1 &&
           task->transfer_mode == XR_TRANSFER_SHARE &&
           task->ownership_action == XR_SEM_OPERAND_BORROW &&
           task->parameter_mode == XR_PARAM_READ && task->access == XR_CALL_ARG_PLAIN &&
           task->origin == XI_PLACE_ORIGIN_NONE && task->lifetime == XI_PLACE_LIFETIME_NONE &&
           task->escape == XI_PLACE_ESCAPE_NONE && task->flags == 0;
}

#endif  // XR_SEMANTIC_TASK_SHAPE_H
