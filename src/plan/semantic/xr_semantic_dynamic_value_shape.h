/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_dynamic_value_shape.h - Values whose type is the untyped reference
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

/* The facts every producer of an untyped reference must state the same way,
 * whatever the opcode: it results in a value, it is not a call, not an import,
 * not an intrinsic, not a view, and does not alias an operand or a parameter.
 * What differs between producers -- whether a constant backs the value, whether
 * metadata describes it -- is left to the roster below. */
static inline bool
xr_semantic_dynamic_value_common_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    return plan && operation && operation->result_value != XR_SEMANTIC_INDEX_NONE &&
           operation->function < xr_semantic_plan_function_count(plan) &&
           operation->callable_function == XR_SEMANTIC_INDEX_NONE &&
           operation->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NONE &&
           operation->allocation_key == NULL &&
           xr_stable_id_equal(operation->allocation_id, zero) &&
           operation->result_alias_operand == -1 && operation->return_parameter == -1 &&
           operation->view_source_value == XR_SEMANTIC_INDEX_NONE &&
           operation->view_element_type == XR_SEMANTIC_INDEX_NONE &&
           operation->view_source_operand == -1 && operation->view_source_parameter == -1 &&
           operation->view_origin == XI_VIEW_ORIGIN_NONE && operation->view_capability == 0 &&
           operation->view_lifetime == 0 && operation->view_complete == 0;
}

/* The roster of producers admitted to this family, and what each must state.
 * A producer is admitted only after its own shape has been measured, so the
 * list grows one measured opcode at a time rather than by opening the family
 * to whatever carries the untyped type.
 *
 * XI_PHI       a join the plan could not narrow to one type: it merges what its
 *              edges already carry, so it names no constant and no metadata.
 * XI_CONST     an enum declaration's namespace descriptor, marked as such by
 *              lowering.  A constant backs it and the member table describes
 *              it, so both are required to be present rather than absent.
 * XI_GET_SHARED a read of a module-level slot, which holds a tagged value and
 *              nothing else.  Its immediate names which slot, so unlike the
 *              other producers it is expected to carry one, and the read
 *              borrows what the slot owns rather than owning it. */
static inline bool
xr_semantic_dynamic_value_producer_is_exact(const XrSemanticOperationRecord *operation) {
    if (!operation)
        return false;
    switch (operation->opcode) {
        case XI_PHI:
            return operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->semantic_immediate == 0 &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE;
        case XI_CONST:
            return operation->auxiliary_kind == XI_AUX_KIND_ENUM_NAMESPACE &&
                   operation->metadata_count != 0 && operation->semantic_immediate == 0 &&
                   operation->constant != XR_SEMANTIC_INDEX_NONE;
        case XI_GET_SHARED:
            return operation->auxiliary_kind == XI_AUX_KIND_NONE &&
                   operation->metadata_count == 0 && operation->operand_count == 0 &&
                   operation->semantic_immediate <= UINT16_MAX &&
                   operation->constant == XR_SEMANTIC_INDEX_NONE &&
                   operation->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
                   operation->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
                   operation->return_complete == 1;
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

static inline bool xr_semantic_dynamic_value_is_exact(const XrSemanticPlan *plan,
                                                      const XrSemanticOperationRecord *operation) {
    return xr_semantic_dynamic_value_producer_is_exact(operation) &&
           xr_semantic_dynamic_value_common_is_exact(plan, operation) &&
           xr_semantic_dynamic_value_type_is_exact(
               xr_semantic_plan_type(plan, operation->result_type));
}

#endif /* XR_SEMANTIC_DYNAMIC_VALUE_SHAPE_H */
