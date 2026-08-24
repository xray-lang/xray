/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_dynamic_value_shape.h - Values carried in generic tagged reference storage
 */

#ifndef XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H
#define XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* The compiler's own "unknown" reference type: source can neither write it nor
 * name it, so a record carrying it was produced by the compiler and nowhere
 * else.  A join over differently shaped arms leaves one behind; so does an enum
 * declaration, whose namespace descriptor has no surface type to carry.
 *
 * Whatever produced it, the value is a reference on every path that can reach
 * it, so it is held the one way every untyped reference is held -- tagged.  The
 * scalar family classifies this type as not-applicable and moves on without
 * binding anything, which is correct, but it leaves the value with no storage
 * at all and refuses its readers with a diagnostic naming the reader rather
 * than the producer. */
static inline bool xr_semantic_dynamic_value_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->source_enum_key == NULL &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0;
}

/* The wider type test, for a producer whose storage fact does not come from the
 * value's type at all.  A module-level slot is one XrValue whatever it holds,
 * so a read of one is tagged because of where it was read, not because of what
 * was in it -- a Task, a Json, a nullable String all come out of the same slot
 * the same way.
 *
 * A native scalar is excluded: an int in a slot has machine storage of its own
 * and the families that name it answer first.  So are borrowed views and exact
 * aggregates, whose storage is a shape rather than a carrier.
 *
 * So is an enum.  Its layout and member identity are facts a value of it
 * carries, and the plan verifier holds a binding for one to the family that
 * knows them; a slot read cannot answer for those by pointing at the slot. */
static inline bool
xr_semantic_dynamic_value_carrier_type_is_exact(const XrSemanticTypeRecord *type) {
    return type && type->kind != XR_KIND_ENUM && type->scalar_rep == XR_SCALAR_REP_NONE &&
           (type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) != 0 &&
           (type->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0 &&
           (type->flags & (XR_SEM_TYPE_BORROW_VIEW | XR_SEM_TYPE_AGGREGATE_EXACT)) == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0;
}

/* The facts every producer of an untyped reference must state the same way,
 * whatever the opcode: it results in a value, it is not a call, not an import,
 * not an intrinsic, not a view, and does not alias an operand or a parameter.
 * What differs between producers -- whether a constant backs the value, whether
 * metadata describes it, whether an immediate names where it lives, whether it
 * resolves an import -- is left to the roster below. */
static inline bool
xr_semantic_dynamic_value_common_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation) {
    return plan && operation && operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->function < xr_semantic_plan_function_count(plan) &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&

           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->view_source_value == XR_SEMANTIC_INDEX_NONE &&
           operation->view_element_type == XR_SEMANTIC_INDEX_NONE &&
           operation->view_source_operand == -1 && operation->view_source_parameter == -1 &&
           operation->view_origin == XI_VIEW_ORIGIN_NONE && operation->view_capability == 0 &&
           operation->view_lifetime == 0 && operation->view_complete == 0;
}

/* Whether the producer allocated what it holds.  A producer that allocates
 * carries the identity of its allocation and a producer that does not carries
 * none, and the roster states which of the two each one is rather than letting
 * either pass unexamined. */
static inline bool xr_semantic_dynamic_value_allocates(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->allocation_key != NULL &&
           !xr_stable_id_equal(operation->allocation_id, zero);
}

static inline bool
xr_semantic_dynamic_value_allocates_nothing(const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return operation && operation->allocation_key == NULL &&
           xr_stable_id_equal(operation->allocation_id, zero);
}

/* The roster of producers admitted to this family, and what each must state.
 * A producer is admitted only after its own shape has been measured, so the
 * list grows one measured opcode at a time rather than by opening the family
 * to whatever carries the untyped type.
 *
 * XI_PHI       a join of reference-capable values. It merges tagged carriers its
 *              incoming edges already own, so it names no constant or metadata.
 *              Unlike the producer-only cases below, a join may preserve an
 *              exact source type such as Array<T> or a class instance; storage
 *              is still the same tagged reference carrier.
 * XI_CONST     an enum declaration's namespace descriptor, marked as such by
 *              lowering.  A constant backs it and the member table describes
 *              it, so both are required to be present rather than absent.
 * XI_GET_SHARED a read of a module-level slot, which holds a tagged value and
 *              nothing else.  Its immediate names which slot, so unlike the
 *              other producers it is expected to carry one, and the read
 *              borrows what the slot owns rather than owning it.
 * XI_AWAIT     the value a finished task handed back, which crosses the
 *              coroutine boundary as one XrValue whatever its type -- so like a
 *              slot read it is judged by the carrier rather than by the type.
 * XI_CELL_NEW  a fresh cell holding a captured binding.  It allocates, which
 *              every other producer here does not, and says so.
 * XI_AS        a checked conversion whose target the plan records as one piece
 *              of metadata; the immediate names which type.
 * XI_IMPORT_REF a reference to something another module owns.  It is the one
 *              producer here that resolves an import, and it borrows what the
 *              other module owns rather than owning it.
 * XI_CORO_OP   a coroutine primitive's result, which leaves the coroutine the
 *              same way an awaited value does -- so it too is judged by the
 *              carrier rather than by the type. */
static inline bool
xr_semantic_dynamic_value_producer_is_exact(const XrSemanticOperationRecord *operation) {
    if (!operation)
        return false;
    switch (operation->opcode) {
        case XI_PHI:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->semantic_immediate == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_CONST:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_ENUM_NAMESPACE &&
                   operation->metadata_count != 0 && operation->semantic_immediate == 0 &&
                   operation->constant != XR_SEMANTIC_INDEX_NONE &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_GET_SHARED:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 0 &&
                   operation->semantic_immediate <= UINT16_MAX &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                   operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_AWAIT:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 1 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_CELL_NEW:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 1 &&
                   operation->semantic_immediate == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates(operation);
        case XI_AS:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 1 && operation->operand_count == 1 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_IMPORT_REF:
            return operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 2 && operation->operand_count == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                   operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        case XI_CORO_OP:
            return operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
                   operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_OWNED &&
                   operation->return_provenance == XR_SEM_RETURN_OWNED &&
                   operation->return_complete == 1 &&
                   xr_semantic_dynamic_value_allocates_nothing(operation);
        default:
            return false;
    }
}

/* Whether a producer of this family owns what it holds or only borrows it.
 * A join and a descriptor own their value; a read of a shared slot borrows the
 * one the slot owns, and releasing it would drop a reference the reader never
 * took.  The builder writes this into the row and both verifiers check it, so
 * like the slot role it is answered here once. */
static inline bool
xr_semantic_dynamic_value_is_borrowed(const XrSemanticOperationRecord *operation) {
    return operation && operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED;
}

/* Which slot role a producer of this family takes.  A join is held in the slot
 * role joins use; every other producer is a temporary.  The builder writes the
 * role, the plan verifier checks it, and the AOT oracle checks it again, so the
 * question is asked in three places and answered here once. */
static inline bool xr_semantic_dynamic_value_is_join(const XrSemanticOperationRecord *operation) {
    return operation && operation->opcode == XI_PHI;
}

/* Re-prove the typed SSA source behind one incoming edge. Target consumers call
 * this only after the whole SemanticPlan verifier has accepted CFG dominance;
 * this local judgement still checks the identity facts that decide whether the
 * incoming value can occupy the same tagged carrier as the PHI result. */
static inline bool xr_semantic_reference_phi_input_is_exact(const XrSemanticPlan *plan,
                                                            uint32_t value, uint32_t type,
                                                            uint32_t function) {
    bool found = false;
    size_t operation_count = xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *definition = xr_semantic_plan_operation(plan, i);
        if (!definition || definition->result_value != value)
            continue;
        if (found || definition->result_type != type || definition->function != function)
            return false;
        found = true;
    }
    size_t parameter_count = xr_semantic_plan_parameter_count(plan);
    for (uint32_t i = 0; i < parameter_count; i++) {
        const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(plan, i);
        if (!parameter || parameter->value != value)
            continue;
        if (found || parameter->type != type || parameter->function != function)
            return false;
        found = true;
    }
    return found;
}

/* A PHI may preserve an exact source type while merging reference-capable
 * values. It is a generic tagged carrier only when every incoming edge carries
 * that same type in the same function and the operation has the generated,
 * ownership-consuming PHI shape. This keeps the broad carrier reusable without
 * admitting a mixed or forged join. */
static inline bool xr_semantic_reference_phi_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticBlockRecord *block =
        operation ? xr_semantic_plan_block(plan, operation->block) : NULL;
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!operation || !block || !operands || operation->opcode != XI_PHI ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation) ||
        !xr_semantic_dynamic_value_producer_is_exact(operation) ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(type) ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->operand_count == 0 || operation->operand_count != block->predecessor_count ||
        operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->effects != xi_generated_op_effects(XI_PHI) ||
        operation->flags != xi_generated_op_default_flags(XI_PHI) ||
        operation->ownership_use != xi_generated_op_own_use(XI_PHI) ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_PHI) ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ || operation->parameter_ownership != XI_OWN_NONE ||
        operation->evidence[0] != 0 || operation->evidence[1] != 0 ||
        operation->evidence[2] != 0 || operation->evidence[3] != 0 ||
        operation->evidence[4] != 0 || operation->evidence[5] != 0 ||
        operation->evidence[6] != 0 || operation->evidence[7] != XR_SEMANTIC_INDEX_NONE ||
        operation->array_element_storage != 0 ||
        operation->array_hof_kind != XR_SEM_ARRAY_HOF_NONE ||
        operation->array_result_element_storage != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *operand = &operands[operation->operand_begin + i];
        if (operand->type != operation->result_type || operand->role != XR_SEM_OPERAND_VALUE ||
            operand->parameter != -1 || operand->transfer_mode != XR_TRANSFER_SHARE ||
            operand->ownership_action != XR_SEM_OPERAND_CONSUME ||
            operand->parameter_mode != XR_PARAM_READ || operand->access != XR_CALL_ARG_PLAIN ||
            operand->origin != XI_PLACE_ORIGIN_NONE ||
            operand->lifetime != XI_PLACE_LIFETIME_NONE ||
            operand->escape != XI_PLACE_ESCAPE_NONE || operand->flags != 0 ||
            !xr_semantic_reference_phi_input_is_exact(
                plan, operand->value, operation->result_type, operation->function))
            return false;
    }
    return true;
}

static inline bool xr_semantic_dynamic_value_is_exact(const XrSemanticPlan *plan,
                                                       const XrSemanticOperationRecord *operation) {
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (operation && operation->opcode == XI_PHI)
        return xr_semantic_reference_phi_is_exact(plan, operation);
    if (!xr_semantic_dynamic_value_producer_is_exact(operation) ||
        !xr_semantic_dynamic_value_common_is_exact(plan, operation))
        return false;
    /* A join, slot read, await or coroutine result is tagged because of the
     * carrier it propagates. The other producers manufacture the compiler's
     * untyped reference value and are held to that narrower type. */
    return (operation->opcode == XI_GET_SHARED || operation->opcode == XI_AWAIT ||
            operation->opcode == XI_CORO_OP)
               ? xr_semantic_dynamic_value_carrier_type_is_exact(type)
               : xr_semantic_dynamic_value_type_is_exact(type);
}

#endif /* XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H */
