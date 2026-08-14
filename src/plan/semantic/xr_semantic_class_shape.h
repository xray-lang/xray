/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_class_shape.h - Shared exactness judgement for source class objects
 *
 * KEY CONCEPT:
 *   A declared class lowers to one allocation whose result carries no class
 *   type: the value is typed `any`, so nothing in the type table names the
 *   declaration. The only authority that binds the allocation to a declaration
 *   is the plan's own source-class table matched by the operation's own class
 *   name. Every layer that has to answer "is this value a source class object"
 *   asks this one judgement, so the target builder, the target verifier and the
 *   AOT representation oracle cannot drift into three similar-looking rules.
 */

#ifndef XR_SEMANTIC_CLASS_SHAPE_H
#define XR_SEMANTIC_CLASS_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

/* The class object value is a freshly owned module-level allocation. Its type
 * is the erased `any` reference, so the type row proves only that the value is
 * a reference-capable ownership root carrying no aggregate geometry. */
static inline bool xr_semantic_class_object_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* The operation shape of a class allocation: no operands, generated effects,
 * flags and ownership, a fresh allocation identity, and at least the class name
 * in its metadata. Anything that deviates is not this family's to claim. */
static inline bool xr_semantic_class_object_operation_is_exact(
    const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->opcode == XI_CLASS_CREATE && operation->operand_count == 0 &&
           operation->metadata_count >= 1 && operation->allocation_key &&
           !xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 && operation->semantic_immediate == 0 &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->effects == xi_generated_op_effects(XI_CLASS_CREATE) &&
           operation->flags == xi_generated_op_default_flags(XI_CLASS_CREATE) &&
           operation->ownership_use == xi_generated_op_own_use(XI_CLASS_CREATE) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
           operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_OWNED &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           operation->view_complete == 0 && operation->view_source_operand == -1 &&
           operation->view_source_parameter == -1;
}

/* The declaration this allocation builds, or XR_SEMANTIC_INDEX_NONE when the
 * plan cannot name exactly one. The match is by class name because that is the
 * only class identity the operation retains; a name that names two declarations
 * or none names nothing, and the caller must refuse rather than guess. */
static inline uint32_t xr_semantic_class_object_source_class(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    if (!plan || !xr_semantic_class_object_operation_is_exact(operation))
        return XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_class_object_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)))
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!metadata || operation->metadata_begin >= metadata_count ||
        operation->metadata_count > metadata_count - operation->metadata_begin)
        return XR_SEMANTIC_INDEX_NONE;
    const char *name = metadata[operation->metadata_begin];
    if (!name || !name[0])
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t class_count = (uint32_t) xr_semantic_plan_source_class_count(plan);
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < class_count; i++) {
        const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, i);
        if (!record || !record->name || strcmp(record->name, name) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE)
            return XR_SEMANTIC_INDEX_NONE;
        match = i;
    }
    if (match == XR_SEMANTIC_INDEX_NONE)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceClassRecord *record = xr_semantic_plan_source_class(plan, match);
    /* A generic skeleton or monomorphized instantiation has no single frozen
     * object identity, so it stays outside this family. */
    if ((record->flags & XR_SEM_SOURCE_CLASS_GENERIC) != 0 || record->ordinal != match ||
        !record->canonical_key || !record->module_path)
        return XR_SEMANTIC_INDEX_NONE;
    return match;
}

static inline bool xr_semantic_class_object_is_exact(const XrSemanticPlan *plan,
                                                     const XrSemanticOperationRecord *operation) {
    return xr_semantic_class_object_source_class(plan, operation) != XR_SEMANTIC_INDEX_NONE;
}

/* The declaration an instance type names, or XR_SEMANTIC_INDEX_NONE. Unlike the
 * class object, an instance keeps its declaration in the type row itself, so
 * the judgement is the row: the reference-capable ownership root that names one
 * frozen declaration and carries no aggregate, enum or scalar geometry. The
 * row's own class identity must agree with the declaration it indexes, so a
 * table whose index and identity disagree names nothing. */
static inline uint32_t xr_semantic_class_instance_type_source_class(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *type) {
    if (!plan || !type || type->kind != XR_KIND_INSTANCE ||
        type->builtin_type != XR_TID_NULL || type->scalar_rep != XR_SCALAR_REP_NONE ||
        type->child_count != 0 || type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->enum_member_count != 0 || type->enum_flags != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        type->source_class == XR_SEMANTIC_INDEX_NONE ||
        type->source_class >= (uint32_t) xr_semantic_plan_source_class_count(plan))
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticSourceClassRecord *record =
        xr_semantic_plan_source_class(plan, type->source_class);
    if (!record || (record->flags & XR_SEM_SOURCE_CLASS_GENERIC) != 0 ||
        record->ordinal != type->source_class || !record->canonical_key ||
        !record->module_path || !xr_stable_id_equal(type->source_class_identity, record->id))
        return XR_SEMANTIC_INDEX_NONE;
    return type->source_class;
}

/* The generated shape of a module-level shared load: a borrowed static read of
 * one slot with no operand, metadata, constant, callee or allocation of its
 * own. Which slot it reads is the caller's question; that it is nothing but a
 * shared read is this judgement's. */
static inline bool xr_semantic_class_shared_read_shape_is_exact(
    const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->opcode == XI_GET_SHARED && operation->operand_count == 0 &&
           operation->metadata_count == 0 && operation->semantic_immediate >= 0 &&
           operation->semantic_immediate <= UINT16_MAX && !operation->allocation_key &&
           xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->constant == XR_SEMANTIC_INDEX_NONE &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->auxiliary_kind == 0 &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           operation->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           operation->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           operation->result_ownership == xi_generated_op_result_ownership(XI_GET_SHARED) &&
           operation->transfer_mode == 0 && operation->parameter_mode == 0 &&
           operation->parameter_ownership == 0 && operation->result_alias_operand == -1 &&
           operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           operation->return_parameter == -1 && operation->return_complete == 1 &&
           operation->view_complete == 0 && operation->view_source_operand == -1 &&
           operation->view_source_parameter == -1;
}

/* The generated shape of the store that fills a shared slot: one consumed plain
 * value, no result of its own, and nothing else. */
static inline bool xr_semantic_class_shared_store_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    if (!plan || !operation || operation->opcode != XI_SET_SHARED ||
        operation->operand_count != 1 || operation->metadata_count != 0 ||
        operation->semantic_immediate < 0 || operation->semantic_immediate > UINT16_MAX ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_SET_SHARED) ||
        operation->flags != xi_generated_op_default_flags(XI_SET_SHARED) ||
        operation->ownership_use != xi_generated_op_own_use(XI_SET_SHARED) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_SET_SHARED) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1)
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return false;
    const XrSemanticOperandRecord *stored = &operands[operation->operand_begin];
    return stored->role == XR_SEM_OPERAND_VALUE && stored->parameter == -1 &&
           stored->transfer_mode == 0 && stored->ownership_action == XR_SEM_OPERAND_CONSUME &&
           stored->parameter_mode == 0 && stored->access == 0 && stored->origin == 0 &&
           stored->lifetime == 0 && stored->escape == 0 && stored->flags == 0;
}

/* The one operation in the module whose result is `value`, or NULL when the
 * module defines it zero times or more than once. */
static inline const XrSemanticOperationRecord *xr_semantic_class_value_definition(
    const XrSemanticPlan *plan, uint32_t value) {
    if (!plan || value == XR_SEMANTIC_INDEX_NONE)
        return NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    const XrSemanticOperationRecord *found = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != value)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found;
}

/* The one store that fills shared slot `slot`, or NULL when the module fills it
 * zero times, more than once, or through a store this judgement cannot read. A
 * slot written twice carries no single frozen value, so it names nothing. */
static inline const XrSemanticOperationRecord *xr_semantic_class_unique_shared_store(
    const XrSemanticPlan *plan, int64_t slot) {
    if (!plan)
        return NULL;
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    const XrSemanticOperationRecord *found = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->opcode != XI_SET_SHARED ||
            candidate->semantic_immediate != slot)
            continue;
        if (found)
            return NULL;
        found = candidate;
    }
    return found && xr_semantic_class_shared_store_shape_is_exact(plan, found) ? found : NULL;
}

/* The value a shared read loads, proved through the module's single store into
 * the same slot, together with the store's own operand row. */
static inline const XrSemanticOperationRecord *xr_semantic_class_shared_read_definition(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    if (!xr_semantic_class_shared_read_shape_is_exact(operation))
        return NULL;
    const XrSemanticOperationRecord *store =
        xr_semantic_class_unique_shared_store(plan, operation->semantic_immediate);
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands =
        store ? xr_semantic_plan_operands(plan, &operand_count) : NULL;
    if (!store || !operands || store->operand_begin >= operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
    /* The slot carries one type: the store's operand row and the read's result
     * must agree, or the read is not reading what the store wrote. */
    if (stored->type != operation->result_type)
        return NULL;
    const XrSemanticOperationRecord *definition =
        xr_semantic_class_value_definition(plan, stored->value);
    return definition && definition->result_value == stored->value &&
                   definition->result_type == stored->type
               ? definition
               : NULL;
}

/* The declaration whose class object this shared read loads, or NONE. */
static inline uint32_t xr_semantic_class_object_read_source_class(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_GET_SHARED ||
        !xr_semantic_class_object_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type)))
        return XR_SEMANTIC_INDEX_NONE;
    return xr_semantic_class_object_source_class(
        plan, xr_semantic_class_shared_read_definition(plan, operation));
}

/* The declaration a construction call builds, or NONE. The call names no callee
 * function and carries no argument: what it constructs is proved from the
 * instance type it returns and from the class object its callee operand loads,
 * and the two must name the same declaration. */
static inline uint32_t xr_semantic_class_construction_source_class(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_CALL || operation->operand_count != 1 ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->flags != xi_generated_op_default_flags(XI_CALL) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_parameter != -1 || operation->return_complete != 1 ||
        operation->view_complete != 0 || operation->view_source_operand != -1 ||
        operation->view_source_parameter != -1)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, operation->result_type));
    if (source_class == XR_SEMANTIC_INDEX_NONE)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 ||
        callee->transfer_mode != 0 || callee->ownership_action != XR_SEM_OPERAND_BORROW ||
        callee->parameter_mode != 0 || callee->access != 0 || callee->origin != 0 ||
        callee->lifetime != 0 || callee->escape != 0 || callee->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *load =
        xr_semantic_class_value_definition(plan, callee->value);
    if (!load || load->result_type != callee->type ||
        xr_semantic_class_object_read_source_class(plan, load) != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    return source_class;
}

/* The declaration whose instance this shared read loads, or NONE. */
static inline uint32_t xr_semantic_class_instance_read_source_class(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_GET_SHARED)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, operation->result_type));
    if (source_class == XR_SEMANTIC_INDEX_NONE ||
        xr_semantic_class_construction_source_class(
            plan, xr_semantic_class_shared_read_definition(plan, operation)) != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    return source_class;
}

/* One judgement for the whole construction family: the borrowed reads of a
 * class object out of its module slot, the owned instance a construction
 * returns, and the borrowed reads of that instance out of its own module slot.
 * Ownership is never assumed from the shape: it is the operation's own result
 * ownership, owned for the construction and borrowed for either read. */
static inline bool xr_semantic_class_instance_value_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *out_source_class) {
    uint32_t source_class = XR_SEMANTIC_INDEX_NONE;
    if (!operation)
        return false;
    if (operation->opcode == XI_CALL)
        source_class = xr_semantic_class_construction_source_class(plan, operation);
    else if (operation->opcode == XI_GET_SHARED) {
        source_class = xr_semantic_class_object_read_source_class(plan, operation);
        if (source_class == XR_SEMANTIC_INDEX_NONE)
            source_class = xr_semantic_class_instance_read_source_class(plan, operation);
    }
    if (source_class == XR_SEMANTIC_INDEX_NONE)
        return false;
    if (out_source_class)
        *out_source_class = source_class;
    return true;
}

/* The declaration whose instance a field read borrows from, or NONE. The read
 * is the generated field load: one borrowed plain operand, one metadata name,
 * and no allocation, constant, callee or view of its own. Which field the name
 * selects is the frontend's proof and is already frozen in the result type; what
 * this judgement adds is that the receiver is an instance this family named, so
 * the read borrows from a proved allocation rather than from an open object. */
static inline uint32_t xr_semantic_class_field_read_source_class(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    if (!plan || xr_semantic_plan_source_class_count(plan) == 0 || !operation ||
        operation->opcode != XI_LOAD_FIELD || operation->operand_count != 1 ||
        operation->metadata_count != 1 || operation->semantic_immediate < 0 ||
        operation->allocation_key || !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != 0 ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->effects != xi_generated_op_effects(XI_LOAD_FIELD) ||
        operation->flags != xi_generated_op_default_flags(XI_LOAD_FIELD) ||
        operation->ownership_use != xi_generated_op_own_use(XI_LOAD_FIELD) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_LOAD_FIELD) ||
        operation->transfer_mode != 0 || operation->parameter_mode != 0 ||
        operation->parameter_ownership != 0 || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->view_complete != 0 ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    if (!operands || operation->operand_begin >= operand_count)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_VALUE || receiver->parameter != -1 ||
        receiver->transfer_mode != 0 || receiver->ownership_action != XR_SEM_OPERAND_BORROW ||
        receiver->parameter_mode != 0 || receiver->access != 0 || receiver->origin != 0 ||
        receiver->lifetime != 0 || receiver->escape != 0 || receiver->flags != 0)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = xr_semantic_class_instance_type_source_class(
        plan, xr_semantic_plan_type(plan, receiver->type));
    const XrSemanticOperationRecord *definition =
        xr_semantic_class_value_definition(plan, receiver->value);
    uint32_t receiver_class = XR_SEMANTIC_INDEX_NONE;
    if (source_class == XR_SEMANTIC_INDEX_NONE || !definition ||
        definition->result_type != receiver->type ||
        !xr_semantic_class_instance_value_is_exact(plan, definition, &receiver_class) ||
        receiver_class != source_class)
        return XR_SEMANTIC_INDEX_NONE;
    return source_class;
}

#endif  // XR_SEMANTIC_CLASS_SHAPE_H
