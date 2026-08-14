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

#endif  // XR_SEMANTIC_CLASS_SHAPE_H
