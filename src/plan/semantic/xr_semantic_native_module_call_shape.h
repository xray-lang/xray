/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_native_module_call_shape.h - Exact native module scalar call shape
 */

#ifndef XR_SEMANTIC_NATIVE_MODULE_CALL_SHAPE_H
#define XR_SEMANTIC_NATIVE_MODULE_CALL_SHAPE_H

#include "../../ir/xi.h"
#include "../../ir/xi_ops_gen.h"
#include "xr_semantic_native_module_shape.h"
#include "xr_semantic_task_shape.h"
#include "../../stdlib/xstdlib_metadata.h"

/* The frozen shape of a native stdlib module member call: a method call whose
 * receiver is a module namespace handle rather than a constructible value, with
 * every argument and the result crossing the boundary as one plain scalar.
 *
 * This judgement had four copies -- SemanticPlan verifier, TargetPlan builder
 * and verifier, AOT refinement -- and the copies disagreed. Two were byte
 * identical, one carried an extra function-window check, and the verifier spelt
 * the same terms inline against raw plan arrays. None of the four restated the
 * auxiliary kind, so a term the classifier requires had no witness in the frozen
 * record. A judgement written four times is a judgement that can drift four
 * ways, and the end-to-end suites cannot see the drift: they compare printed
 * output, which stays correct while a copy quietly admits or refuses more than
 * its siblings. One definition is what keeps the layers answering the same
 * question.
 *
 * The auxiliary kind is the term that motivated collecting them. The classifier
 * proves `aux_kind == XI_AUX_KIND_NONE` over the Xi value (the selector is a
 * plain spelling and not some other auxiliary payload), and the builder freezes
 * that fact into `auxiliary_kind`. A frozen row that never restates it would let
 * a build-time fact stand in as an admission term with no witness in the
 * artifact the consumers actually read. Restating it here costs nothing for a
 * plan this builder produced and closes the gap for one it did not.
 *
 * `semantic_immediate` is the frozen `XI_CALL_METHOD.aux_int`, which encodes
 * `(method_symbol << 1) | optional_chaining`. Requiring it positive and even
 * states that the selector resolved and that the callsite is not an optional
 * chain, whose short circuit this row does not describe. */
static inline bool xr_semantic_native_module_scalar_call_shape_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *operation,
    const char **out_selector, uint32_t *out_receiver_value, uint32_t *out_arity) {
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!plan || !operation || !operands || !metadata || operation->opcode != XI_CALL_METHOD ||
        operation->semantic_immediate <= 0 || (operation->semantic_immediate & 1) != 0 ||
        operation->auxiliary_kind != XI_AUX_KIND_NONE || operation->operand_count == 0 ||
        operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        (operation->flags & XI_FLAG_MAY_SUSPEND) != 0 ||
        operation->effects != xi_generated_op_effects(XI_CALL_METHOD) ||
        operation->result_alias_operand != -1 ||
        operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_CALL_RESULT ||
        !xr_semantic_native_module_boundary_type_is_exact(
            xr_semantic_plan_type(plan, operation->result_type), true))
        return false;
    const XrSemanticOperandRecord *receiver = &operands[operation->operand_begin];
    if (receiver->role != XR_SEM_OPERAND_RECEIVER || receiver->parameter != -1 ||
        receiver->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
        receiver->ownership_action != XR_SEM_OPERAND_BORROW)
        return false;
    for (uint16_t i = 1; i < operation->operand_count; i++) {
        const XrSemanticOperandRecord *argument = receiver + i;
        if (argument->role != XR_SEM_OPERAND_ARGUMENT || argument->parameter != (int16_t) (i - 1) ||
            argument->flags != XR_SEM_OPERAND_CALL_CONTRACT ||
            !xr_semantic_native_module_boundary_type_is_exact(
                xr_semantic_plan_type(plan, argument->type), false))
            return false;
    }
    if (out_selector)
        *out_selector = metadata[operation->metadata_begin];
    if (out_receiver_value)
        *out_receiver_value = receiver->value;
    if (out_arity)
        *out_arity = (uint32_t) (operation->operand_count - 1u);
    return true;
}

/* The module-init rows behind a namespace receiver, rebuilt from the frozen
 * plan alone. These three had four copies each -- SemanticPlan verifier,
 * TargetPlan builder and verifier, AOT refinement -- and the copies had already
 * begun to part: the verifier reached into `plan->metadata` directly where the
 * other three went through the bounds-checked accessor, so the layer meant to
 * trust nothing carried one guard fewer than the layer that builds. Collecting
 * them keeps that from happening again, and the accessor form is the one kept. */

/* The module-init import reference of a native stdlib namespace. Its frozen
 * import classification is resolved against the native definition registry
 * rather than against a compiled module, and its metadata pair names the
 * module path with an empty member, so a member import and a source-module
 * namespace both stay outside this authority. */
static inline bool
xr_semantic_native_module_import_is_exact(const XrSemanticPlan *plan,
                                          const XrSemanticOperationRecord *record,
                                          const char **out_module_path) {
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(plan, record->result_type) : NULL;
    if (!record || !type || !metadata || record->opcode != XI_IMPORT_REF || record->function != 0 ||
        record->operand_count != 0 || record->metadata_count != 2 ||
        record->metadata_begin + 1u >= metadata_count ||
        record->import_resolution != XR_SEM_IMPORT_RESOLUTION_NATIVE_STDLIB ||
        record->semantic_immediate < -1 || record->semantic_immediate > UINT16_MAX ||
        record->allocation_key || !xr_semantic_shape_stable_id_is_zero(record->allocation_id) ||
        record->constant != XR_SEMANTIC_INDEX_NONE ||
        record->callable_function != XR_SEMANTIC_INDEX_NONE || record->auxiliary_kind != 0 ||
        record->effects != xi_generated_op_effects(XI_IMPORT_REF) ||
        record->flags != xi_generated_op_default_flags(XI_IMPORT_REF) ||
        record->ownership_use != xi_generated_op_own_use(XI_IMPORT_REF) ||
        record->result_ownership != XI_GEN_RESULT_OWNERSHIP_BORROWED ||
        record->result_alias_operand != -1 ||
        record->return_provenance != XR_SEM_RETURN_BORROWED_STATIC ||
        record->return_parameter != -1 || record->return_complete != 1 ||
        type->scalar_rep != XR_SCALAR_REP_NONE || type->child_count != 0 ||
        type->aggregate_extent != 0 || type->aggregate_align != 0 ||
        type->flags != (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT))
        return false;
    const char *module_path = metadata[record->metadata_begin];
    const char *member = metadata[record->metadata_begin + 1u];
    if (!module_path || !member || member[0] != '\0' ||
        !xr_stdlib_metadata_module_known(module_path))
        return false;
    if (out_module_path)
        *out_module_path = module_path;
    return true;
}

/* The shared-slot read that republishes the namespace inside a function. */
static inline bool
xr_semantic_native_module_load_is_exact(const XrSemanticPlan *plan,
                                        const XrSemanticOperationRecord *record) {
    const XrSemanticTypeRecord *type =
        record ? xr_semantic_plan_type(plan, record->result_type) : NULL;
    return record && type && record->opcode == XI_GET_SHARED && record->operand_count == 0 &&
           record->metadata_count == 0 && record->semantic_immediate >= 0 &&
           record->semantic_immediate <= UINT16_MAX && !record->allocation_key &&
           xr_semantic_shape_stable_id_is_zero(record->allocation_id) &&
           record->constant == XR_SEMANTIC_INDEX_NONE &&
           record->callable_function == XR_SEMANTIC_INDEX_NONE && record->auxiliary_kind == 0 &&
           record->import_resolution == XR_SEM_IMPORT_RESOLUTION_NONE &&
           record->effects == xi_generated_op_effects(XI_GET_SHARED) &&
           record->flags == xi_generated_op_default_flags(XI_GET_SHARED) &&
           record->ownership_use == xi_generated_op_own_use(XI_GET_SHARED) &&
           record->result_ownership == XI_GEN_RESULT_OWNERSHIP_BORROWED &&
           record->result_alias_operand == -1 &&
           record->return_provenance == XR_SEM_RETURN_BORROWED_STATIC &&
           record->return_parameter == -1 && record->return_complete == 1 &&
           type->scalar_rep == XR_SCALAR_REP_NONE && type->child_count == 0 &&
           type->aggregate_extent == 0 && type->aggregate_align == 0 &&
           type->flags == (XR_SEM_TYPE_REFERENCE_CAPABLE | XR_SEM_TYPE_OWNERSHIP_ROOT);
}

/* Rebuilt from the frozen rows: the load reads a module shared slot, exactly
 * one module-init store publishes that slot, and the stored value is the
 * module-init import reference above. The returned module path is the frozen
 * metadata string, never a backend guess. */
static inline const char *xr_semantic_native_module_namespace_path(const XrSemanticPlan *plan,
                                                                   uint32_t receiver_value) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &operand_count);
    uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(plan);
    const XrSemanticOperationRecord *load = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != receiver_value)
            continue;
        if (load)
            return NULL;
        load = candidate;
    }
    if (!xr_semantic_native_module_load_is_exact(plan, load))
        return NULL;
    const XrSemanticOperationRecord *store = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->opcode != XI_SET_SHARED || candidate->function != 0 ||
            candidate->semantic_immediate != load->semantic_immediate)
            continue;
        if (store)
            return NULL;
        store = candidate;
    }
    if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
        return NULL;
    const XrSemanticOperandRecord *stored = &operands[store->operand_begin];
    if (stored->role != XR_SEM_OPERAND_VALUE || stored->parameter != -1 ||
        stored->ownership_action != XR_SEM_OPERAND_CONSUME || stored->flags != 0 ||
        stored->type != load->result_type)
        return NULL;
    const XrSemanticOperationRecord *import = NULL;
    for (uint32_t i = 0; i < operation_count; i++) {
        const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(plan, i);
        if (!candidate || candidate->result_value != stored->value)
            continue;
        if (import)
            return NULL;
        import = candidate;
    }
    const char *module_path = NULL;
    return import && import->result_type == load->result_type &&
                   xr_semantic_native_module_import_is_exact(plan, import, &module_path)
               ? module_path
               : NULL;
}

#endif /* XR_SEMANTIC_NATIVE_MODULE_CALL_SHAPE_H */
