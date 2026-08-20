/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_dynamic_phi_shape.h - Exact merge of dynamic values at a join
 */

#ifndef XR_SEMANTIC_DYNAMIC_PHI_SHAPE_H
#define XR_SEMANTIC_DYNAMIC_PHI_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_plan.h"

/* The value a join produces when its incoming edges carry references the plan
 * could not narrow to one type -- what a `match` whose arms yield different
 * shapes leaves behind.  The type is the compiler's own "unknown", which no
 * declaration can present: source can neither write it nor name it, so a record
 * carrying it came from a merge and nowhere else.
 *
 * The scalar family classifies this type as not-applicable and moves on without
 * binding anything, which is correct -- it is not a scalar -- but it left the
 * value with no storage at all, and every later reader of it was refused with
 * a diagnostic naming the reader rather than the merge.  A reference is a
 * reference on every incoming edge, so the merge carries the tagged value the
 * edges already carry. */
static inline bool xr_semantic_dynamic_phi_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation) {
    XrStableId zero = {{0}};
    const XrSemanticTypeRecord *type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || operation->opcode != XI_PHI || !type ||
        operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->function >= xr_semantic_plan_function_count(plan) ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE || operation->allocation_key != NULL ||
        !xr_stable_id_equal(operation->allocation_id, zero) ||
        operation->result_alias_operand != -1 || operation->return_parameter != -1 ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0)
        return false;
    /* The merged type is the unknown reference spelling and nothing else: a
     * narrowed join is some other family's to bind, and admitting one here
     * would take a value that family already answers for. */
    return type->kind == XR_KIND_UNKNOWN && type->builtin_type == XR_TID_NULL &&
           type->child_count == 0 && type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->source_class == XR_SEMANTIC_INDEX_NONE && type->source_enum_key == NULL &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0;
}

#endif /* XR_SEMANTIC_DYNAMIC_PHI_SHAPE_H */
