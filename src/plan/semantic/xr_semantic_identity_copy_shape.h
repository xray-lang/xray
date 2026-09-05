/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_identity_copy_shape.h - Exact identity copy authority
 */

#ifndef XR_SEMANTIC_IDENTITY_COPY_SHAPE_H
#define XR_SEMANTIC_IDENTITY_COPY_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../shared/xr_copy_core.h"
#include "xr_semantic_array_type_shape.h"
#include "xr_semantic_plan.h"

/* A copy that only renames its operand: the result holds exactly what the
 * source holds. Unlike the container copy next door, nothing is materialised
 * and no ownership root is created -- the result borrows, which is why the
 * operation is required to say so.
 *
 * Every field the operation carries is enumerated, in the same style as the
 * two specialised identity-copy recipes in the target builder, because
 * anything left unnamed is a difference that could change what the copy means.
 *
 * Deliberately not enumerated: the semantic type. Those two recipes pin theirs
 * because each serves exactly one type; this one serves whatever the source
 * already proved, and the source's own family is what decided that.
 *
 * Read by the target builder, to give the result the source's representation,
 * and by the target verifier, to accept that binding. One statement of the
 * shape, so the two layers cannot drift apart.
 */
static inline const XrSemanticOperandRecord *
xr_semantic_identity_copy_source(const XrSemanticPlan *plan,
                                 const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!plan || !operation || !operands || operation->opcode != XI_COPY ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->semantic_immediate != XI_COPY_KIND_IDENTITY || operation->allocation_key ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE || operation->auxiliary_kind != 0 ||
        operation->metadata_count != 0 || operation->effects != xi_generated_op_effects(XI_COPY) ||
        operation->flags != xi_generated_op_default_flags(XI_COPY) ||
        operation->ownership_use != xi_generated_op_own_use(XI_COPY) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_alias_operand != 0)
        return NULL;
    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    if (source->role != XR_SEM_OPERAND_VALUE || source->parameter != -1 || source->flags != 0)
        return NULL;
    return source;
}

static inline bool xr_semantic_identity_copy_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation,
                                                      uint32_t *source_value_out) {
    const XrSemanticOperandRecord *source =
        xr_semantic_identity_copy_source(plan, operation);
    if (!source || source->type != operation->result_type)
        return false;
    if (source_value_out)
        *source_value_out = source->value;
    return true;
}

/* `value!` over a nullable Array does not copy or materialise the container.
 * Lowering emits an ISNULL guard and lets the false edge enter a one-predecessor
 * block whose identity COPY merely narrows the reference type.  That control
 * edge is the proof: no other cross-type COPY is storage identity.
 *
 * This judgement intentionally freezes the complete local proof instead of
 * treating "nullable to non-null" as sufficient.  Both types must be the exact
 * compiler-owned Array row with the same element, the source must be the value
 * tested by the unique ISNULL producer, and the COPY must be in that guard's
 * false successor. */
static inline bool xr_semantic_checked_nullable_array_identity_copy_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *source_value_out) {
    XrStableId zero = {{0}};
    const XrSemanticOperandRecord *source =
        xr_semantic_identity_copy_source(plan, operation);
    const XrSemanticTypeRecord *source_type =
        source ? xr_semantic_plan_type(plan, source->type) : NULL;
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    uint32_t type_child_count = 0;
    const uint32_t *type_children = xr_semantic_plan_type_children(plan, &type_child_count);
    if (!source || !source_type || !result_type || !type_children ||
        !xr_semantic_array_type_row_is_exact(source_type) ||
        !xr_semantic_array_type_row_is_exact(result_type) ||
        (source_type->flags & XR_SEM_TYPE_NULLABLE) == 0 ||
        (result_type->flags & XR_SEM_TYPE_NULLABLE) != 0 ||
        (source_type->flags & (uint8_t) ~XR_SEM_TYPE_NULLABLE) != result_type->flags ||
        source_type->child_begin >= type_child_count ||
        result_type->child_begin >= type_child_count ||
        type_children[source_type->child_begin] != type_children[result_type->child_begin] ||
        source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_BORROW ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || operation->allocation_key ||
        !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->array_element_storage != 0 || operation->reserved_view[0] != 0 ||
        operation->reserved_view[1] != 0 || operation->array_hof_kind != 0 ||
        operation->array_result_element_storage != 0 || operation->evidence[0] != 0 ||
        operation->evidence[1] != 0 || operation->evidence[2] != 0 ||
        operation->evidence[3] != 0 || operation->evidence[4] != 0 ||
        operation->evidence[5] != 0 || operation->evidence[6] != 0 ||
        operation->evidence[7] != XR_SEMANTIC_INDEX_NONE ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation ||
        operation->block >= xr_semantic_plan_block_count(plan))
        return false;

    const XrSemanticBlockRecord *copy_block = xr_semantic_plan_block(plan, operation->block);
    uint32_t predecessor_count = 0;
    const uint32_t *predecessors = xr_semantic_plan_predecessors(plan, &predecessor_count);
    if (!copy_block || !predecessors || copy_block->function != operation->function ||
        copy_block->predecessor_count != 1 ||
        copy_block->predecessor_begin >= predecessor_count)
        return false;
    uint32_t guard_block_index = predecessors[copy_block->predecessor_begin];
    const XrSemanticBlockRecord *guard_block = xr_semantic_plan_block(plan, guard_block_index);
    if (!guard_block || guard_block->function != operation->function ||
        guard_block->kind != XI_BLOCK_IF || guard_block->successors[1] != operation->block ||
        guard_block->control_value == XR_SEMANTIC_INDEX_NONE)
        return false;

    const XrSemanticOperationRecord *guard =
        xr_semantic_unique_value_definition(plan, guard_block->control_value);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!guard || !operands || guard->function != operation->function ||
        guard->block != guard_block_index || guard->opcode != XI_ISNULL ||
        guard->result_value != guard_block->control_value || guard->operand_count != 1 ||
        guard->operand_begin >= operand_count || guard->metadata_count != 0 ||
        guard->semantic_immediate != 0 || guard->allocation_key ||
        !xr_stable_id_equal(guard->allocation_id, zero) ||
        guard->constant != XR_SEMANTIC_INDEX_NONE ||
        guard->callable_function != XR_SEMANTIC_INDEX_NONE ||
        guard->auxiliary_kind != XI_AUX_KIND_NONE ||
        guard->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        guard->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        guard->effects != xi_generated_op_effects(XI_ISNULL) ||
        guard->flags != xi_generated_op_default_flags(XI_ISNULL) ||
        guard->ownership_use != xi_generated_op_own_use(XI_ISNULL) ||
        guard->result_ownership != xi_generated_op_result_ownership(XI_ISNULL) ||
        guard->transfer_mode != XR_TRANSFER_SHARE || guard->parameter_mode != XR_PARAM_READ ||
        guard->parameter_ownership != XI_OWN_NONE || guard->result_alias_operand != -1 ||
        guard->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        guard->view_element_type != XR_SEMANTIC_INDEX_NONE || guard->view_source_operand != -1 ||
        guard->view_source_parameter != -1 || guard->view_origin != XI_VIEW_ORIGIN_NONE ||
        guard->view_capability != 0 || guard->view_lifetime != 0 || guard->view_complete != 0 ||
        guard->array_element_storage != 0 || guard->reserved_view[0] != 0 ||
        guard->reserved_view[1] != 0 || guard->array_hof_kind != 0 ||
        guard->array_result_element_storage != 0 || guard->evidence[0] != 0 ||
        guard->evidence[1] != 0 || guard->evidence[2] != 0 || guard->evidence[3] != 0 ||
        guard->evidence[4] != 0 || guard->evidence[5] != 0 || guard->evidence[6] != 0 ||
        guard->evidence[7] != XR_SEMANTIC_INDEX_NONE)
        return false;
    const XrSemanticOperandRecord *tested = &operands[guard->operand_begin];
    if (tested->value != source->value || tested->type != source->type ||
        tested->role != XR_SEM_OPERAND_VALUE || tested->parameter != -1 ||
        tested->transfer_mode != XR_TRANSFER_SHARE ||
        tested->ownership_action != XR_SEM_OPERAND_BORROW ||
        tested->parameter_mode != XR_PARAM_READ || tested->access != XR_CALL_ARG_PLAIN ||
        tested->origin != XI_PLACE_ORIGIN_NONE || tested->lifetime != XI_PLACE_LIFETIME_NONE ||
        tested->escape != XI_PLACE_ESCAPE_NONE || tested->flags != 0)
        return false;
    if (source_value_out)
        *source_value_out = source->value;
    return true;
}

/* Storage identity is either an ordinary same-type rename or the exact
 * checked Array narrowing above.  Target construction and independent target
 * verification consume this combined authority. */
static inline bool xr_semantic_storage_identity_copy_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *source_value_out) {
    return xr_semantic_identity_copy_is_exact(plan, operation, source_value_out) ||
           xr_semantic_checked_nullable_array_identity_copy_is_exact(plan, operation,
                                                                      source_value_out);
}

#endif  // XR_SEMANTIC_IDENTITY_COPY_SHAPE_H
