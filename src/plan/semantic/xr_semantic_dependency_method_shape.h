/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_dependency_method_shape.h - Frozen dependency method binding
 */
#ifndef XR_SEMANTIC_DEPENDENCY_METHOD_SHAPE_H
#define XR_SEMANTIC_DEPENDENCY_METHOD_SHAPE_H

#include "xr_semantic_plan.h"
#include "../../ir/xi.h"
#include <string.h>

/* Resolve the declaration only. Open dispatch still requires a graph proof or
 * a verified virtual dispatch table before execution can select a body. */
static inline const XrSemanticSourceMethodRecord *
xr_semantic_dependency_method_is_exact(const XrSemanticPlan *primary,
                                        const XrSemanticCallTargetRecord *target,
                                        const XrSemanticPlan *dependency) {
    if (!primary || !target || !dependency ||
        target->kind != XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY)
        return NULL;
    const XrSemanticDependencyRecord *required =
        xr_semantic_plan_dependency(primary, target->dependency);
    const XrSemanticEntityRecord *module = xr_semantic_plan_unique_module_entity(dependency);
    const XrSemanticOperationRecord *operation =
        xr_semantic_plan_operation(primary, target->operation);
    const XrSemanticTypeRecord *receiver = xr_semantic_plan_type(primary, target->callable_type);
    uint32_t operand_count = 0, metadata_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(primary, &operand_count);
    const char *const *metadata = xr_semantic_plan_metadata(primary, &metadata_count);
    if (!required || !module || !operation || !receiver || !operands || !metadata ||
        !xr_stable_id_equal(required->module, module->id) ||
        !xr_fingerprint_equal(required->semantic_fingerprint,
                              xr_semantic_plan_fingerprint(dependency)) ||
        operation->opcode != XI_CALL_METHOD || operation->operand_count == 0 ||
        operation->operand_begin >= operand_count ||
        operation->operand_count > operand_count - operation->operand_begin ||
        operation->metadata_count != 1 || operation->metadata_begin >= metadata_count ||
        !metadata[operation->metadata_begin] ||
        operands[operation->operand_begin].role != XR_SEM_OPERAND_RECEIVER ||
        operands[operation->operand_begin].type != target->callable_type ||
        receiver->source_class != XR_SEMANTIC_INDEX_NONE)
        return NULL;
    const XrSemanticSourceMethodRecord *match = NULL;
    uint32_t count = (uint32_t) xr_semantic_plan_source_method_count(dependency);
    for (uint32_t i = 0; i < count; i++) {
        const XrSemanticSourceMethodRecord *candidate = xr_semantic_plan_source_method(dependency, i);
        if (!candidate || !xr_stable_id_equal(candidate->id, target->export_identity))
            continue;
        if (match)
            return NULL;
        match = candidate;
    }
    const XrSemanticSourceClassRecord *source_class = match
        ? xr_semantic_plan_source_class(dependency, match->source_class) : NULL;
    uint8_t flags = XR_SEM_SOURCE_METHOD_INSTANCE;
    if (source_class && (source_class->flags & XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL) == 0)
        flags |= XR_SEM_SOURCE_METHOD_OPEN_DOMAIN;
    return match && source_class && match->name && match->flags == flags &&
           match->function < xr_semantic_plan_function_count(dependency) &&
           match->parameter_count == operation->operand_count &&
           strcmp(match->name, metadata[operation->metadata_begin]) == 0 &&
           (source_class->flags & XR_SEM_SOURCE_CLASS_RUNTIME_TYPE) != 0 &&
           (source_class->flags & XR_SEM_SOURCE_CLASS_GENERIC) == 0 &&
           xr_stable_id_equal(source_class->id, receiver->source_class_identity)
               ? match : NULL;
}

#endif // XR_SEMANTIC_DEPENDENCY_METHOD_SHAPE_H
