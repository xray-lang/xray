/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_map_entry_iterator_shape.h - Frozen Map entry-iterator authority
 */

#ifndef XR_SEMANTIC_MAP_ENTRY_ITERATOR_SHAPE_H
#define XR_SEMANTIC_MAP_ENTRY_ITERATOR_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <string.h>

static inline bool xr_semantic_plain_builtin_type(const XrSemanticTypeRecord *type,
                                                  uint32_t kind, uint16_t children,
                                                  uint32_t extent, uint8_t flags) {
    XrStableId zero = {{0}};
    return type && type->kind == kind && type->builtin_type == XR_TID_NULL &&
           type->source_class == XR_SEMANTIC_INDEX_NONE &&
           xr_stable_id_equal(type->source_class_identity, zero) &&
           xr_stable_id_equal(type->source_enum_identity, zero) && !type->source_enum_key &&
           type->child_count == children && type->aggregate_extent == extent &&
           type->aggregate_align == 0 && type->enum_layout_id == 0 &&
           type->enum_member_count == 0 && type->enum_flags == 0 && type->reserved_enum == 0 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->flags == flags && type->canonical_key;
}

static inline bool xr_semantic_map_entry_iterator_types_are_exact(
    const XrSemanticPlan *plan, const XrSemanticTypeRecord *map,
    const XrSemanticTypeRecord *iterator, uint32_t *entry_type_index) {
    uint32_t child_count = 0;
    const uint32_t *children = xr_semantic_plan_type_children(plan, &child_count);
    if (!plan || !children ||
        !xr_semantic_plain_builtin_type(
            map, XR_KIND_MAP, 2, 0,
            XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        !xr_semantic_plain_builtin_type(
            iterator, XR_KIND_INSTANCE, 1, 0,
            XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        map->child_begin > child_count || map->child_count > child_count - map->child_begin ||
        iterator->child_begin >= child_count)
        return false;
    uint32_t entry_index = children[iterator->child_begin];
    const XrSemanticTypeRecord *entry = xr_semantic_plan_type(plan, entry_index);
    if (!xr_semantic_plain_builtin_type(
            entry, XR_KIND_TUPLE, 2, 2,
            XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT) ||
        entry->child_begin > child_count || entry->child_count > child_count - entry->child_begin ||
        children[entry->child_begin] != children[map->child_begin] ||
        children[entry->child_begin + 1] != children[map->child_begin + 1])
        return false;
    const char prefix[] = "type-v3:11:0:0:0:0:0:0:0:0:255:0:;named:8:Iterator[1;";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t key_length = strlen(iterator->canonical_key);
    size_t entry_key_length = strlen(entry->canonical_key);
    if (key_length != prefix_length + entry_key_length + 1u ||
        strncmp(iterator->canonical_key, prefix, prefix_length) != 0 ||
        memcmp(iterator->canonical_key + prefix_length, entry->canonical_key,
               entry_key_length) != 0 ||
        iterator->canonical_key[key_length - 1u] != ']')
        return false;
    if (entry_type_index)
        *entry_type_index = entry_index;
    return true;
}

static inline bool xr_semantic_map_entry_iterator_common_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    XrSemanticIntrinsicKind intrinsic, XiMethodSymbolId symbol, uint8_t result_ownership,
    uint8_t return_provenance, uint8_t return_complete, uint32_t *receiver_value) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata ||
        operation->intrinsic_kind != intrinsic || operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate != ((int64_t) symbol << 1) ||
        operation->operand_count != 1 || operation->operand_begin >= operand_count ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] || operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        operation->constant != XR_SEMANTIC_INDEX_NONE ||
        operation->callable_function != XR_SEMANTIC_INDEX_NONE ||
        operation->import_resolution != XR_SEM_IMPORT_RESOLUTION_NONE ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        (operation->flags != xi_generated_op_default_flags(XI_CALL_METHOD) &&
         operation->flags != (xi_generated_op_default_flags(XI_CALL_METHOD) | XI_FLAG_TAIL)) ||
        operation->ownership_use != xi_generated_op_own_use(XI_CALL_METHOD) ||
        operation->result_ownership != result_ownership ||
        operation->transfer_mode != XR_TRANSFER_SHARE || operation->parameter_mode != XR_PARAM_READ ||
        operation->parameter_ownership != XI_OWN_NONE || operation->result_alias_operand != -1 ||
        operation->return_parameter != -1 || operation->return_provenance != return_provenance ||
        operation->return_complete != return_complete ||
        operation->view_source_value != XR_SEMANTIC_INDEX_NONE ||
        operation->view_element_type != XR_SEMANTIC_INDEX_NONE ||
        operation->view_source_operand != -1 || operation->view_source_parameter != -1 ||
        operation->view_origin != XI_VIEW_ORIGIN_NONE || operation->view_capability != 0 ||
        operation->view_lifetime != 0 || operation->view_complete != 0 ||
        operation->reserved_view[0] != 0 || operation->reserved_view[1] != 0)
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->transfer_mode != XR_TRANSFER_SHARE ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW || receiver->parameter_mode != 0 ||
        receiver->access != 0 || receiver->origin != 0 || receiver->lifetime != 0 ||
        receiver->escape != 0 || receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT)
        return false;
    if (receiver_value)
        *receiver_value = receiver->value;
    return true;
}

static inline const XrSemanticOperationRecord *xr_semantic_map_entry_unique_value_definition(
    const XrSemanticPlan *plan, uint32_t value) {
    const XrSemanticOperationRecord *definition = NULL;
    size_t count = xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != value)
            continue;
        if (definition)
            return NULL;
        definition = candidate;
    }
    return definition;
}

static inline bool xr_semantic_map_entries_iterator_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value, uint32_t *entry_type_index) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_map_entry_iterator_common_is_exact(
            plan, operation, XR_SEM_INTRINSIC_MAP_ENTRIES_ITERATOR,
            XI_METHOD_SYMBOL_ENTRIES_ITERATOR, XI_GEN_RESULT_OWNERSHIP_OWNED,
            XR_SEM_RETURN_OWNED, 1, &receiver))
        return false;
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const XrSemanticOperandRecord *receiver_operand = &operands[operation->operand_begin];
    const XrSemanticTypeRecord *map = xr_semantic_plan_type(plan, receiver_operand->type);
    const XrSemanticTypeRecord *iterator = xr_semantic_plan_type(plan, operation->result_type);
    if (!xr_semantic_map_entry_iterator_types_are_exact(plan, map, iterator, entry_type_index))
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static inline bool xr_semantic_map_entry_iterator_has_next_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_map_entry_iterator_common_is_exact(
            plan, operation, XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_HAS_NEXT,
            XI_METHOD_SYMBOL_HAS_NEXT, XI_GEN_RESULT_OWNERSHIP_CALL_RESULT,
            XR_SEM_RETURN_NONE, 0, &receiver))
        return false;
    const XrSemanticOperationRecord *factory =
        xr_semantic_map_entry_unique_value_definition(plan, receiver);
    const XrSemanticTypeRecord *result = xr_semantic_plan_type(plan, operation->result_type);
    XrStableId zero = {{0}};
    if (!factory || factory->function != operation->function ||
        !xr_semantic_map_entries_iterator_is_exact(plan, factory, NULL, NULL) || !result ||
        result->kind != XR_KIND_BOOL || result->builtin_type != XR_TID_NULL ||
        result->source_class != XR_SEMANTIC_INDEX_NONE ||
        !xr_stable_id_equal(result->source_class_identity, zero) || result->child_count != 0 ||
        result->aggregate_extent != 0 || result->aggregate_align != 0 ||
        result->scalar_rep != XR_SCALAR_REP_NONE || result->flags != 0)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

static inline bool xr_semantic_map_entry_iterator_next_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    uint32_t *receiver_value) {
    uint32_t receiver = XR_SEMANTIC_INDEX_NONE;
    if (!xr_semantic_map_entry_iterator_common_is_exact(
            plan, operation, XR_SEM_INTRINSIC_MAP_ENTRY_ITERATOR_NEXT, XI_METHOD_SYMBOL_NEXT,
            XI_GEN_RESULT_OWNERSHIP_OWNED, XR_SEM_RETURN_OWNED, 1, &receiver))
        return false;
    const XrSemanticOperationRecord *factory =
        xr_semantic_map_entry_unique_value_definition(plan, receiver);
    uint32_t entry_type = XR_SEMANTIC_INDEX_NONE;
    if (!factory || factory->function != operation->function ||
        !xr_semantic_map_entries_iterator_is_exact(plan, factory, NULL, &entry_type) ||
        operation->result_type != entry_type)
        return false;
    if (receiver_value)
        *receiver_value = receiver;
    return true;
}

#endif  // XR_SEMANTIC_MAP_ENTRY_ITERATOR_SHAPE_H
