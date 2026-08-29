/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_native_leaf_shape.h - Exact direct native leaf call authority
 */

#ifndef XR_SEMANTIC_NATIVE_LEAF_SHAPE_H
#define XR_SEMANTIC_NATIVE_LEAF_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_native_module_shape.h"
#include "../../stdlib/xstdlib_metadata.h"
#include <stdio.h>

static inline bool xr_semantic_native_target_leaf_identity(const XrStdlibDefEntry *entry,
                                                           XrStableId *out) {
    if (!entry || !out || entry->target_leaf <= XR_STDLIB_TARGET_LEAF_NONE ||
        entry->target_leaf >= XR_STDLIB_TARGET_LEAF_COUNT)
        return false;
    char key[512];
    int written = snprintf(key, sizeof(key), "stdlib-target-leaf-v1:%u:%s.%s:%s",
                           (unsigned) entry->target_leaf, entry->module, entry->name,
                           entry->signature);
    XrFingerprint digest;
    return written > 0 && (size_t) written < sizeof(key) &&
           xr_stable_id_from_key(key, out, &digest);
}

static inline const XrSemanticOperationRecord *
xr_semantic_unique_operation_for_value(const XrSemanticPlan *plan, uint32_t function,
                                       uint32_t value) {
    const XrSemanticOperationRecord *match = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->function != function || candidate->result_value != value)
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    return match;
}

/* Resolve a private native declaration used through the module's shared slot.
 * This is existence authority, independent of whether the registry marks the
 * member yieldable.  The module initializer publishes one grounded member
 * import, a function reads that exact slot, and the call consumes the read as
 * its callee. */
static inline const XrSemanticOperationRecord *
xr_semantic_native_direct_import_for_value(const XrSemanticPlan *plan, uint32_t function,
                                           uint32_t value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    for (uint32_t depth = 0; plan && operands && depth < operation_count; depth++) {
        const XrSemanticOperationRecord *producer =
            xr_semantic_unique_operation_for_value(plan, function, value);
        if (!producer)
            return NULL;
        if (producer->opcode == XI_IMPORT_REF)
            return producer;
        if (producer->opcode == XI_COPY &&
            producer->semantic_immediate == XI_COPY_KIND_IDENTITY &&
            producer->operand_count == 1 && producer->operand_begin < operand_count &&
            producer->result_alias_operand == 0) {
            value = operands[producer->operand_begin].value;
            continue;
        }
        if (producer->opcode != XI_GET_SHARED || producer->semantic_immediate < 0 ||
            producer->operand_count != 0)
            return NULL;
        const XrSemanticOperationRecord *store = NULL;
        for (uint32_t i = 0; i < operation_count; i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
            if (!candidate || candidate->opcode != XI_SET_SHARED || candidate->function != 0 ||
                candidate->semantic_immediate != producer->semantic_immediate)
                continue;
            if (store)
                return NULL;
            store = candidate;
        }
        if (!store || store->operand_count != 1 || store->operand_begin >= operand_count ||
            operands[store->operand_begin].type != producer->result_type)
            return NULL;
        function = 0;
        value = operands[store->operand_begin].value;
    }
    return NULL;
}

static inline bool xr_semantic_native_direct_scalar_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrStdlibDefEntry **out_entry) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL ||
        operation->operand_count == 0 || operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(result_type, true))
        return false;
    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 || callee->flags != 0 ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    const XrSemanticOperationRecord *import = xr_semantic_native_direct_import_for_value(
        plan, operation->function, callee->value);
    if (!import || import->opcode != XI_IMPORT_REF ||
        (import->function != 0 && import->function != operation->function) ||
        import->operand_count != 0 || import->metadata_count != 2 ||
        import->metadata_begin >= metadata_count || import->metadata_begin + 1u >= metadata_count ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        import->intrinsic_kind != XR_SEM_INTRINSIC_NONE ||
        import->auxiliary_kind != XI_AUX_KIND_NONE ||
        import->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        (import->flags & XI_FLAG_MAY_SUSPEND) != 0 || import->result_type != callee->type)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = callee + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT ||
            argument->parameter != (int16_t) (i - 1u) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != XR_SEM_OPERAND_BORROW ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_direct_member(
        metadata[import->metadata_begin], metadata[import->metadata_begin + 1u],
        (uint16_t) (operation->operand_count - 1u));
    if (!entry || entry->target_leaf != XR_STDLIB_TARGET_LEAF_NONE)
        return false;
    if (out_entry)
        *out_entry = entry;
    return true;
}

/* A direct private native leaf is a grounded XI_IMPORT_REF used as the callee
 * of XI_CALL. The generated registry must opt the member into one typed leaf
 * kind; ordinary direct members, yieldable members, source imports, and open
 * callables remain outside. The call row carries only scalar arguments and one
 * scalar result, so it creates no ownership or suspension authority. */
static inline bool xr_semantic_native_target_leaf_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrStdlibDefEntry **out_entry, XrStableId *out_identity) {
    uint32_t operand_count = 0;
    uint32_t metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *result_type =
        operation ? xr_semantic_plan_type(plan, operation->result_type) : NULL;
    if (!plan || !operation || !operands || !metadata ||
        operation->opcode != XI_CALL || operation->operand_count == 0 ||
        operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 0 || operation->semantic_immediate != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !result_type || result_type->kind != XR_KIND_INT ||
        !xr_semantic_native_module_boundary_type_is_exact(result_type, true))
        return false;

    const XrSemanticOperandRecord *callee = &operands[operation->operand_begin];
    if (callee->role != XR_SEM_OPERAND_CALLEE || callee->parameter != -1 || callee->flags != 0 ||
        callee->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    const XrSemanticOperationRecord *import = xr_semantic_unique_operation_for_value(
        plan, operation->function, callee->value);
    if (!import || import->opcode != XI_IMPORT_REF || import->operand_count != 0 ||
        import->metadata_count != 2 || import->metadata_begin >= metadata_count ||
        import->metadata_begin + 1u >= metadata_count ||
        import->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        import->intrinsic_kind != XR_SEM_INTRINSIC_NONE || import->auxiliary_kind != XI_AUX_KIND_NONE ||
        import->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        (import->flags & XI_FLAG_MAY_SUSPEND) != 0)
        return false;

    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = callee + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT ||
            argument->parameter != (int16_t) (i - 1u) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            argument->ownership_action != XR_SEM_OPERAND_BORROW ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }

    const char *module = metadata[import->metadata_begin];
    const char *member = metadata[import->metadata_begin + 1u];
    const XrStdlibDefEntry *entry = xr_stdlib_metadata_exact_native_target_leaf(
        module, member, (uint16_t) (operation->operand_count - 1u));
    XrStableId identity = {{0}};
    if (!entry || !xr_semantic_native_target_leaf_identity(entry, &identity))
        return false;
    if (out_entry)
        *out_entry = entry;
    if (out_identity)
        *out_identity = identity;
    return true;
}

static inline bool xr_semantic_native_target_leaf_call_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const XrStdlibDefEntry **out_entry, XrStableId *out_identity) {
    return operation &&
           operation->intrinsic_kind == XR_SEM_INTRINSIC_NATIVE_TARGET_LEAF_SCALAR_CALL &&
           xr_semantic_native_target_leaf_call_shape_is_exact(plan, operation, out_entry,
                                                               out_identity);
}

#endif /* XR_SEMANTIC_NATIVE_LEAF_SHAPE_H */
