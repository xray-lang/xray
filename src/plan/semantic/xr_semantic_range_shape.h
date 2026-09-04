/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_range_shape.h - Exact compiler-owned Range value authority
 */

#ifndef XR_SEMANTIC_RANGE_SHAPE_H
#define XR_SEMANTIC_RANGE_SHAPE_H

#include "xr_semantic_plan.h"
#include "xr_semantic_shared_read_shape.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

/* A compiler-owned nominal has no source declaration identity. The canonical
 * key therefore proves both its spelling and its closed type-argument list; a
 * user declaration with the same name necessarily carries a source-class row. */
static inline bool xr_semantic_compiler_nominal_type_is_exact(const XrSemanticTypeRecord *type,
                                                              const char *name) {
    XrStableId zero = {{0}};
    char expected[128];
    int length = name ? snprintf(expected, sizeof(expected),
                                 "type-v3:%u:0:0:0:0:0:0:0:0:%u:0:;named:%zu:%s[0]",
                                 (unsigned) XR_KIND_INSTANCE, (unsigned) XR_SCALAR_REP_NONE,
                                 strlen(name), name)
                      : -1;
    return type && length > 0 && (size_t) length < sizeof(expected) &&
           type->kind == XR_KIND_INSTANCE && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) &&
           type->canonical_key && strcmp(type->canonical_key, expected) == 0;
}

static inline bool xr_semantic_range_type_is_exact(const XrSemanticTypeRecord *type) {
    return xr_semantic_compiler_nominal_type_is_exact(type, "Range");
}

static inline bool xr_semantic_range_bound_type_is_exact(const XrSemanticTypeRecord *type) {
    XrStableId zero = {{0}};
    return type && type->kind == XR_KIND_INT && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->scalar_rep == XR_NATIVE_I64 && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->flags == 0;
}

/* XI_RANGE is the one construction route for the compiler-owned Range value.
 * Bounds stay in the scalar family; this shape freezes the owned tagged result
 * and its inclusive-end bit without inferring either from a later method use. */
static inline bool xr_semantic_range_value_is_exact(const XrSemanticPlan *plan,
                                                    const XrSemanticOperationRecord *operation) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticTypeRecord *result =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    const XrSemanticFunctionRecord *function =
        operation ? xr_semantic_plan_function(plan, operation->function) : NULL;
    XrStableId zero = {{0}};
    if (!plan || !operation || !operands || !function || !xr_semantic_range_type_is_exact(result) ||
        operation->opcode != XI_RANGE || operation->result_value == XR_SEMANTIC_INDEX_NONE ||
        operation->result_value < function->value_begin ||
        operation->result_value >= function->value_begin + function->value_count ||
        operation->operand_count != 2 || operation->operand_begin > operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        (operation->semantic_immediate != 0 && operation->semantic_immediate != 1) ||
        operation->metadata_count != 0 || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_RANGE) ||
        operation->flags != xi_generated_op_default_flags(XI_RANGE) ||
        operation->ownership_use != xi_generated_op_own_use(XI_RANGE) ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED ||
        operation->result_ownership != xi_generated_op_result_ownership(XI_RANGE) ||
        operation->transfer_mode != XR_TRANSFER_SHARE ||
        operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != XR_SEM_RETURN_OWNED ||
        operation->return_complete != 1 || operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0 ||
        operation->array_element_storage != 0 || operation->array_hof_kind != 0 ||
        operation->array_result_element_storage != 0 || operation->allocation_key ||
        !xr_stable_id_equal(operation->allocation_id, zero) ||
        xr_semantic_unique_value_definition(plan, operation->result_value) != operation)
        return false;
    for (uint16_t i = 0; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *bound = &operands[operation->operand_begin + i];
        if (!xr_semantic_range_bound_type_is_exact(xr_semantic_plan_type(plan, bound->type)) ||
            bound->role != XR_SEM_OPERAND_VALUE || bound->parameter != -1 ||
            bound->transfer_mode != XR_TRANSFER_SHARE ||
            bound->ownership_action != XR_SEM_OPERAND_BORROW || bound->parameter_mode != 0 ||
            bound->access != 0 || bound->origin != 0 || bound->lifetime != 0 ||
            bound->escape != 0 || bound->flags != 0)
            return false;
    }
    return true;
}

#endif /* XR_SEMANTIC_RANGE_SHAPE_H */
