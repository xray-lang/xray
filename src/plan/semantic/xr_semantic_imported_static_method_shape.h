/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_imported_static_method_shape.h - Imported static declarations
 */
#ifndef XR_SEMANTIC_IMPORTED_STATIC_METHOD_SHAPE_H
#define XR_SEMANTIC_IMPORTED_STATIC_METHOD_SHAPE_H

#include "xr_semantic_class_shape.h"

/* The namespace operand proves which exported class owns the declaration. It
 * never supplies an instance parameter and never needs a virtual dispatch seal. */
static inline bool xr_semantic_imported_static_namespace_is_exact(
    const XrSemanticPlan *plan, const XrSemanticOperationRecord *call,
    const char **module_path, const char **member) {
    if (!plan || !call || call->opcode != XI_CALL_METHOD ||
        (call->semantic_immediate & 1) != 0 || call->metadata_count != 1 ||
        call->operand_count == 0)
        return false;
    uint32_t count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(plan, &count);
    const char *const *metadata = xr_semantic_plan_metadata(plan, &metadata_count);
    if (!operands || call->operand_begin >= count ||
        call->operand_count > count - call->operand_begin || !metadata ||
        call->metadata_begin >= metadata_count || !metadata[call->metadata_begin] ||
        !metadata[call->metadata_begin][0])
        return false;
    const XrSemanticOperandRecord *receiver = &operands[call->operand_begin];
    return receiver->role == XR_SEM_OPERAND_RECEIVER && receiver->parameter == -1 &&
           receiver->ownership_action == XR_SEM_OPERAND_BORROW &&
           receiver->parameter_mode == 0 && receiver->transfer_mode == 0 &&
           receiver->access == 0 && receiver->origin == 0 && receiver->lifetime == 0 &&
           receiver->escape == 0 && receiver->flags == XR_SEM_OPERAND_CALL_CONTRACT &&
           xr_semantic_imported_class_value_is_exact(
               plan, receiver->value, receiver->type, module_path, member);
}

static inline uint32_t xr_semantic_imported_static_method_function(
    const XrSemanticPlan *caller, const XrSemanticOperationRecord *call,
    const XrSemanticPlan *dependency, const XrSemanticSourceExportRecord *exported) {
    const char *path = NULL, *member = NULL;
    if (!xr_semantic_imported_static_namespace_is_exact(caller, call, &path, &member) ||
        !dependency || !exported || !exported->name || strcmp(member, exported->name) != 0)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t source_class = xr_semantic_source_class_export_source_class(dependency, exported);
    const XrSemanticSourceClassRecord *declaration =
        xr_semantic_plan_source_class(dependency, source_class);
    if (!declaration || (declaration->flags & XR_SEM_SOURCE_CLASS_GENERIC) != 0)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t metadata_count = 0;
    const char *const *metadata = xr_semantic_plan_metadata(caller, &metadata_count);
    const char *selector = metadata[call->metadata_begin];
    uint32_t match = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < xr_semantic_plan_function_count(dependency); i++) {
        const XrSemanticFunctionRecord *function = xr_semantic_plan_function(dependency, i);
        if (!function || function->source_class != source_class ||
            function->source_kind != XR_SEM_SOURCE_FUNCTION_STATIC_METHOD ||
            !function->name || strcmp(function->name, selector) != 0)
            continue;
        if (match != XR_SEMANTIC_INDEX_NONE ||
            function->parameter_count != call->operand_count - 1u)
            return XR_SEMANTIC_INDEX_NONE;
        match = i;
    }
    return match;
}

/* Close the import proof against the exact dependency snapshot before a
 * downstream consumer uses the declaration's function index. */
static inline uint32_t xr_semantic_dependency_static_method_is_exact(
    const XrSemanticPlan *caller, const XrSemanticCallTargetRecord *target,
    const XrSemanticPlan *dependency) {
    if (!caller || !target || !dependency ||
        target->kind != XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_DEPENDENCY)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticDependencyRecord *required =
        xr_semantic_plan_dependency(caller, target->dependency);
    const XrSemanticEntityRecord *module = xr_semantic_plan_unique_module_entity(dependency);
    const XrSemanticOperationRecord *call = xr_semantic_plan_operation(caller, target->operation);
    const XrSemanticSourceExportRecord *exported =
        xr_semantic_plan_source_export(dependency, target->source_export);
    const char *path = NULL, *member = NULL;
    if (!required || !module || !exported || !required->module_path ||
        !xr_stable_id_equal(required->module, module->id) ||
        !xr_fingerprint_equal(required->semantic_fingerprint,
                              xr_semantic_plan_fingerprint(dependency)) ||
        !xr_stable_id_equal(target->export_identity, exported->id) ||
        target->function != XR_SEMANTIC_INDEX_NONE ||
        target->callable_type != XR_SEMANTIC_INDEX_NONE ||
        !xr_semantic_imported_static_namespace_is_exact(caller, call, &path, &member) ||
        strcmp(required->module_path, path) != 0)
        return XR_SEMANTIC_INDEX_NONE;
    uint32_t index = xr_semantic_imported_static_method_function(caller, call, dependency, exported);
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(dependency, index);
    return function && xr_stable_id_equal(function->id, target->callee_function)
               ? index : XR_SEMANTIC_INDEX_NONE;
}

#endif // XR_SEMANTIC_IMPORTED_STATIC_METHOD_SHAPE_H
