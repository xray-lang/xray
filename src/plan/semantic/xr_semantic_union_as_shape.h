/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_union_as_shape.h - Exact checked conversions out of unions
 */

#ifndef XR_SEMANTIC_UNION_AS_SHAPE_H
#define XR_SEMANTIC_UNION_AS_SHAPE_H

#include "xr_semantic_dynamic_value_shape.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../shared/xr_type_names_core.h"
#include <string.h>

/* XI_AS stores a runtime type id and its display spelling, while the source
 * union stores structural member type records.  Rebuild their agreement from
 * both facts: neither a name alone nor an opcode alone is type authority.  The
 * first admitted cohort is the exact scalar/string family exercised by source
 * union conversions; other runtime-testable types remain closed until their
 * own structural identity can be proved from SemanticPlan. */
static inline bool xr_semantic_union_as_member_matches_target(
    const XrSemanticTypeRecord *member, uint32_t target_tid, const char *target_name) {
    if (!member || !target_name || target_tid >= XR_TID_COUNT || member->builtin_type != XR_TID_NULL ||
        member->child_count != 0 || member->aggregate_extent != 0 ||
        member->aggregate_align != 0 || member->source_class != XR_SEMANTIC_INDEX_NONE ||
        member->source_enum_key != NULL || member->enum_layout_id != 0 ||
        member->enum_member_count != 0 || member->enum_flags != 0 || member->reserved_enum != 0)
        return false;

    const char *public_name = xr_type_name_from_tid((XrTypeId) target_tid);
    if (!public_name || strcmp(target_name, public_name) != 0)
        return false;
    if (member->kind == XR_KIND_INT || member->kind == XR_KIND_FLOAT) {
        const XrExactScalarDesc *scalar = xr_exact_scalar_by_native_type(member->scalar_rep);
        bool target_family_matches = member->kind == XR_KIND_INT
                                         ? XR_TID_IS_INT((XrTypeId) target_tid)
                                         : XR_TID_IS_FLOAT((XrTypeId) target_tid);
        return member->flags == 0 && scalar && target_family_matches &&
               strcmp(scalar->source_name, target_name) == 0;
    }
    return member->kind == XR_KIND_STRING && target_tid == XR_TID_STRING &&
           member->scalar_rep == XR_SCALAR_REP_NONE &&
           member->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* Prove the exact unsafe checked conversion `union_value as member`.  Safe
 * `as?`, non-union sources, targets outside the source union, and target-name
 * or target-id disagreements deliberately receive no authority here. */
static inline bool xr_semantic_union_as_conversion_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *source_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    uint32_t child_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (source_value)
        *source_value = XR_SEMANTIC_INDEX_NONE;
    if (!plan || !operation || !operands || !metadata || !children ||
        !xr_semantic_checked_as_base_is_exact(plan, operation) ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] || operation->semantic_immediate < 0 ||
        (operation->semantic_immediate & INT64_C(1)) != 0 ||
        (uint64_t) operation->semantic_immediate > ((uint64_t) UINT32_MAX << 1) ||
        operation->result_alias_operand != -1)
        return false;

    const XrSemanticOperandRecord *source = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *source_type = xr_semantic_plan_type(plan, source->type);
    if (!source_type || source_type->kind != XR_KIND_UNION || source_type->child_count < 2 ||
        !xr_semantic_dynamic_value_carrier_type_is_exact(source_type) ||
        source_type->child_begin > child_count ||
        source_type->child_count > child_count - source_type->child_begin ||
        source->value == XR_SEMANTIC_INDEX_NONE || source->role != XR_SEM_OPERAND_VALUE ||
        source->parameter != -1 || source->transfer_mode != XR_TRANSFER_SHARE ||
        source->ownership_action != XR_SEM_OPERAND_BORROW ||
        source->parameter_mode != XR_PARAM_READ || source->access != XR_CALL_ARG_PLAIN ||
        source->origin != XI_PLACE_ORIGIN_NONE || source->lifetime != XI_PLACE_LIFETIME_NONE ||
        source->escape != XI_PLACE_ESCAPE_NONE || source->flags != 0)
        return false;

    uint32_t target_tid = (uint32_t) ((uint64_t) operation->semantic_immediate >> 1);
    uint32_t matches = 0;
    for (uint16_t i = 0; i < source_type->child_count; i++) {
        uint32_t member_index = children[source_type->child_begin + i];
        const XrSemanticTypeRecord *member = xr_semantic_plan_type(plan, member_index);
        if (member_index == operation->result_type && xr_semantic_union_as_member_matches_target(
                member, target_tid, metadata[operation->metadata_begin]))
            matches++;
    }
    if (matches != 1)
        return false;
    if (source_value)
        *source_value = source->value;
    return true;
}

#endif /* XR_SEMANTIC_UNION_AS_SHAPE_H */
