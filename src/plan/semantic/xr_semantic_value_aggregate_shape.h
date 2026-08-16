/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_value_aggregate_shape.h - Exact value-aggregate declaration shape
 */

#ifndef XR_SEMANTIC_VALUE_AGGREGATE_SHAPE_H
#define XR_SEMANTIC_VALUE_AGGREGATE_SHAPE_H

#include "xr_semantic_class_shape.h"
#include <string.h>

typedef struct XrSemanticValueAggregateShape {
    uint32_t semantic_type;
    uint32_t source_class;
    uint32_t class_operation;
    uint32_t field_metadata_begin;
    uint16_t field_count;
} XrSemanticValueAggregateShape;

/* Does this type occupy an aggregate slot in its own right?
 *
 *   1  yes -- a tuple, fixed array, value struct, or exact value class
 *   0  no  -- it is some other representation, not an error
 *  -1  the record contradicts itself and no answer is derivable
 *
 * A nullable is never an aggregate: the null discriminator is representation
 * the slot cannot carry. A scalar_rep on an otherwise aggregate kind is the
 * contradiction case -- two representations claimed for one type.
 *
 * This says nothing about the fields. A type can answer 1 here and still be
 * ineligible for aggregate storage because a field is not, which is a separate
 * recursive question each layer answers with its own memo strategy. */
static inline int xr_semantic_aggregate_type_kind(const XrSemanticTypeRecord *type) {
    if (!type)
        return -1;
    if ((type->flags & XR_SEM_TYPE_NULLABLE) != 0)
        return 0;
    if (type->kind == XR_KIND_TUPLE || type->kind == XR_KIND_FIXED_ARRAY)
        return type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1;
    if (type->kind == XR_KIND_STRUCT_OBJECT)
        return (type->flags & XR_SEM_TYPE_VALUE) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    if (type->kind == XR_KIND_INSTANCE)
        return (type->flags & XR_SEM_TYPE_AGGREGATE_EXACT) == 0
                   ? 0
                   : (type->scalar_rep == XR_SCALAR_REP_NONE ? 1 : -1);
    return 0;
}

/* A direct-local call whose result is an aggregate returned by value.
 *
 * The call names the storage of its own result, so the caller's slot is where
 * the callee writes and no ownership transfer is modelled: the value is born
 * in the slot it lives in. That is only true when the return is fresh and
 * whole -- not an alias of an argument, not written through a return
 * parameter, complete rather than partially initialized -- and when the callee
 * states the same return contract the call site reads.
 *
 * The field-level question is deliberately not asked here. This judgement is
 * about the call, and each layer pairs it with its own recursive eligibility
 * walk over the type before granting an aggregate slot. */
static inline bool
xr_semantic_direct_local_aggregate_result_is_exact(const XrSemanticPlan *plan,
                                                   const XrSemanticOperationRecord *operation,
                                                   const XrSemanticFunctionRecord *callee) {
    return plan && operation && callee &&
           (operation->opcode == XI_CALL || operation->opcode == XI_TAIL_CALL) &&
           operation->result_type == callee->return_type &&
           operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->return_complete == 1 && operation->return_provenance == XR_SEM_RETURN_OWNED &&
           callee->return_parameter == -1 && callee->return_provenance == XR_SEM_RETURN_OWNED &&
           xr_semantic_aggregate_type_kind(xr_semantic_plan_type(plan, operation->result_type)) ==
               1;
}

static inline bool xr_semantic_value_aggregate_new_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation, uint32_t semantic_type,
    uint32_t *out_source_class, const XrSemanticOperationRecord **out_class_operation) {
    XrStableId zero = {{0}};
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        plan ? xr_semantic_plan_operands(plan, &operand_count) : NULL;
    const XrSemanticTypeRecord *type = plan ? xr_semantic_plan_type(plan, semantic_type) : NULL;
    if (!plan || !operation || !type || operation->opcode != XI_AGG_NEW ||
        operation->result_type != semantic_type || operation->operand_count != 1 ||
        operation->operand_begin >= operand_count || operation->semantic_immediate != 0 ||
        !operation->allocation_key || xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_AGG_NEW) ||
        operation->flags != xi_generated_op_default_flags(XI_AGG_NEW) ||
        operation->ownership_use != xi_generated_op_own_use(XI_AGG_NEW) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_AGG_NEW) ||
        operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED || operation->return_parameter != -1 ||
        operation->return_complete != 1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        type->kind != XR_KIND_INSTANCE || type->scalar_rep != XR_SCALAR_REP_NONE ||
        (type->flags & (XR_SEM_TYPE_VALUE | XR_SEM_TYPE_AGGREGATE_EXACT)) !=
            (XR_SEM_TYPE_VALUE | XR_SEM_TYPE_AGGREGATE_EXACT) ||
        type->child_count == 0 || type->aggregate_extent != type->child_count)
        return false;
    const XrSemanticOperandRecord *descriptor = &operands[operation->operand_begin];
    if (descriptor->role != XR_SEM_OPERAND_VALUE || descriptor->parameter != -1 ||
        descriptor->flags != 0)
        return false;
    const XrSemanticOperationRecord *load =
        xr_semantic_class_value_definition(plan, descriptor->value);
    uint32_t source_class = xr_semantic_class_object_read_source_class(plan, load);
    const XrSemanticOperationRecord *definition =
        load ? xr_semantic_class_shared_read_definition(plan, load) : NULL;
    if (source_class == XR_SEMANTIC_INDEX_NONE || !definition ||
        xr_semantic_class_object_source_class(plan, definition) != source_class)
        return false;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!metadata || operation->metadata_count != type->child_count ||
        operation->metadata_begin > metadata_count ||
        operation->metadata_count > metadata_count - operation->metadata_begin)
        return false;
    for (uint16_t i = 0; i < type->child_count; i++)
        if (!metadata[operation->metadata_begin + i] || !metadata[operation->metadata_begin + i][0])
            return false;
    if (out_source_class)
        *out_source_class = source_class;
    if (out_class_operation)
        *out_class_operation = definition;
    return true;
}

static inline bool xr_semantic_value_aggregate_shape_for_type(const XrSemanticPlan *plan,
                                                              uint32_t semantic_type,
                                                              XrSemanticValueAggregateShape *out) {
    if (out)
        *out = (XrSemanticValueAggregateShape) {
            .semantic_type = XR_SEMANTIC_INDEX_NONE,
            .source_class = XR_SEMANTIC_INDEX_NONE,
            .class_operation = XR_SEMANTIC_INDEX_NONE,
            .field_metadata_begin = XR_SEMANTIC_INDEX_NONE,
        };
    const XrSemanticTypeRecord *type = plan ? xr_semantic_plan_type(plan, semantic_type) : NULL;
    if (!plan || !type || !out || type->child_count == 0)
        return false;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *class_operation = NULL;
    uint32_t class_operation_index = XR_SEMANTIC_INDEX_NONE;
    uint32_t field_metadata_begin = XR_SEMANTIC_INDEX_NONE;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!metadata)
        return false;
    bool found = false;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(plan, i);
        if (!operation || operation->opcode != XI_AGG_NEW ||
            operation->result_type != semantic_type)
            continue;
        uint32_t candidate_class = XR_SEMANTIC_INDEX_NONE;
        const XrSemanticOperationRecord *candidate_operation = NULL;
        if (!xr_semantic_value_aggregate_new_is_exact(plan, operation, semantic_type,
                                                      &candidate_class, &candidate_operation))
            return false;
        uint32_t candidate_index = XR_SEMANTIC_INDEX_NONE;
        for (uint32_t j = 0; j < operation_count; j++)
            if (xr_semantic_plan_operation(plan, j) == candidate_operation)
                candidate_index = j;
        if (candidate_index == XR_SEMANTIC_INDEX_NONE ||
            (found &&
             (candidate_class != source_class || candidate_index != class_operation_index)))
            return false;
        if (found) {
            for (uint16_t field = 0; field < type->child_count; field++)
                if (strcmp(metadata[field_metadata_begin + field],
                           metadata[operation->metadata_begin + field]) != 0)
                    return false;
        } else {
            field_metadata_begin = operation->metadata_begin;
        }
        found = true;
        source_class = candidate_class;
        class_operation = candidate_operation;
        class_operation_index = candidate_index;
    }
    const XrSemanticSourceClassRecord *declaration =
        found ? xr_semantic_plan_source_class(plan, source_class) : NULL;
    if (!found || !declaration || !class_operation || !metadata)
        return false;
    uint32_t field_begin = field_metadata_begin;
    for (uint16_t i = 0; i < type->child_count; i++)
        if (!metadata[field_begin + i] || !metadata[field_begin + i][0])
            return false;
    *out = (XrSemanticValueAggregateShape) {
        .semantic_type = semantic_type,
        .source_class = source_class,
        .class_operation = class_operation_index,
        .field_metadata_begin = field_begin,
        .field_count = type->child_count,
    };
    return true;
}

#endif  // XR_SEMANTIC_VALUE_AGGREGATE_SHAPE_H
