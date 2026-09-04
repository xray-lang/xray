/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_target_program_reachability.c - Frozen SemanticPlan executable closure
 */

#include "xr_target_program_reachability.h"
#include "../semantic/xr_semantic_class_shape.h"
#include "../semantic/xr_semantic_local_call_target_shape.h"
#include "../../base/xmalloc.h"
#include <stdio.h>
#include <string.h>

static bool reachability_fail(char *error, size_t error_size, const char *detail) {
    if (error && error_size)
        snprintf(error, error_size, "XR_TARGET_1003: %s", detail);
    return false;
}

void xr_target_program_reachability_dispose(XrTargetProgramReachability *reachability) {
    if (!reachability)
        return;
    xr_free(reachability->function_begins);
    xr_free(reachability->functions);
    memset(reachability, 0, sizeof(*reachability));
}

bool xr_target_program_function_is_reachable(const XrTargetProgramReachability *reachability,
                                             uint32_t module, uint32_t function) {
    if (!reachability || !reachability->function_begins || !reachability->functions ||
        module >= reachability->module_count)
        return false;
    uint32_t begin = reachability->function_begins[module];
    uint32_t end = reachability->function_begins[module + 1u];
    return function < end - begin && reachability->functions[begin + function] != 0;
}

static bool reachability_mark(XrTargetProgramReachability *reachability, uint32_t module,
                              uint32_t function, bool *changed) {
    if (!reachability || module >= reachability->module_count)
        return false;
    uint32_t begin = reachability->function_begins[module];
    uint32_t end = reachability->function_begins[module + 1u];
    if (function >= end - begin)
        return false;
    if (!reachability->functions[begin + function]) {
        reachability->functions[begin + function] = 1;
        if (changed)
            *changed = true;
    }
    return true;
}

static bool reachability_dependency_module(const XrSemanticPlan *const *modules,
                                           uint32_t module_count, const XrSemanticPlan *semantic,
                                           uint32_t dependency, uint32_t *out_module) {
    if (out_module)
        *out_module = UINT32_MAX;
    const XrSemanticDependencyRecord *required =
        semantic && dependency < xr_semantic_plan_dependency_count(semantic)
            ? xr_semantic_plan_dependency(semantic, dependency)
            : NULL;
    uint32_t match = UINT32_MAX;
    for (uint32_t module = 0; required && module < module_count; module++) {
        const XrSemanticEntityRecord *entity =
            modules[module] ? xr_semantic_plan_unique_module_entity(modules[module]) : NULL;
        if (!entity || !xr_stable_id_equal(entity->id, required->module) ||
            !xr_fingerprint_equal(xr_semantic_plan_fingerprint(modules[module]),
                                  required->semantic_fingerprint))
            continue;
        if (match != UINT32_MAX)
            return false;
        match = module;
    }
    if (match == UINT32_MAX || !out_module)
        return false;
    *out_module = match;
    return true;
}

static bool reachability_mark_target(const XrSemanticPlan *const *modules, uint32_t module_count,
                                     uint32_t caller_module,
                                     const XrSemanticCallTargetRecord *target,
                                     const XrSemanticOperationRecord *operation,
                                     XrTargetProgramReachability *reachability, bool *changed) {
    const XrSemanticPlan *semantic = modules[caller_module];
    uint32_t local_function_count = (uint32_t) xr_semantic_plan_function_count(semantic);
    if (xr_semantic_call_target_names_local_function(target, operation, local_function_count))
        return reachability_mark(reachability, caller_module, target->function, changed);

    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT) {
        uint32_t dependency_module = UINT32_MAX;
        if (!reachability_dependency_module(modules, module_count, semantic, target->dependency,
                                            &dependency_module))
            return false;
        const XrSemanticPlan *dependency = modules[dependency_module];
        const XrSemanticSourceExportRecord *source_export =
            target->source_export < xr_semantic_plan_source_export_count(dependency)
                ? xr_semantic_plan_source_export(dependency, target->source_export)
                : NULL;
        return source_export && source_export->kind == XR_SEM_SOURCE_EXPORT_FUNCTION &&
               reachability_mark(reachability, dependency_module, source_export->function, changed);
    }

    if (target->kind != XR_SEM_CALL_TARGET_SOURCE_CLASS_CONSTRUCTOR)
        return true;

    uint32_t constructor = XR_SEMANTIC_INDEX_NONE;
    uint32_t constructor_module = caller_module;
    if (target->dependency == XR_SEMANTIC_INDEX_NONE) {
        uint32_t source_class = xr_semantic_class_construction_source_class(semantic, operation);
        constructor = xr_semantic_class_constructor_function(semantic, source_class);
    } else {
        if (!reachability_dependency_module(modules, module_count, semantic, target->dependency,
                                            &constructor_module))
            return false;
        const XrSemanticPlan *dependency = modules[constructor_module];
        const XrSemanticSourceExportRecord *source_export =
            target->source_export < xr_semantic_plan_source_export_count(dependency)
                ? xr_semantic_plan_source_export(dependency, target->source_export)
                : NULL;
        uint32_t source_class = xr_semantic_imported_class_construction_authority_source_class(
            semantic, dependency, xr_semantic_plan_dependency(semantic, target->dependency),
            source_export, operation, &constructor);
        if (source_class == XR_SEMANTIC_INDEX_NONE)
            return false;
    }
    /* A class with no declared constructor enters no body. */
    return constructor == XR_SEMANTIC_INDEX_NONE ||
           reachability_mark(reachability, constructor_module, constructor, changed);
}

bool xr_target_program_reachability_build(const XrSemanticPlan *const *modules,
                                          uint32_t module_count, XrTargetProgramReachability *out,
                                          char *error, size_t error_size) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!modules || !module_count || !out)
        return reachability_fail(error, error_size,
                                 "program executable reachability input is incomplete");
    uint32_t *begins = (uint32_t *) xr_calloc((size_t) module_count + 1u, sizeof(*begins));
    if (!begins)
        return reachability_fail(error, error_size,
                                 "program executable reachability allocation failed");
    for (uint32_t module = 0; module < module_count; module++) {
        size_t count = modules[module] ? xr_semantic_plan_function_count(modules[module]) : 0u;
        if (!modules[module] || count > UINT32_MAX - begins[module]) {
            xr_free(begins);
            return reachability_fail(error, error_size,
                                     "program executable reachability function set is invalid");
        }
        begins[module + 1u] = begins[module] + (uint32_t) count;
    }
    uint8_t *functions =
        (uint8_t *) xr_calloc(begins[module_count] ? begins[module_count] : 1u, sizeof(*functions));
    if (!functions) {
        xr_free(begins);
        return reachability_fail(error, error_size,
                                 "program executable reachability allocation failed");
    }
    *out = (XrTargetProgramReachability) {
        .module_count = module_count,
        .function_begins = begins,
        .functions = functions,
    };

    bool changed = false;
    for (uint32_t module = 0; module < module_count; module++) {
        uint32_t count = (uint32_t) xr_semantic_plan_function_count(modules[module]);
        for (uint32_t function = 0; function < count; function++) {
            const XrSemanticFunctionRecord *record =
                xr_semantic_plan_function(modules[module], function);
            if (record && record->is_module_initializer &&
                !reachability_mark(out, module, function, &changed))
                goto invalid;
        }
    }

    do {
        changed = false;
        for (uint32_t module = 0; module < module_count; module++) {
            const XrSemanticPlan *semantic = modules[module];
            uint32_t target_count = (uint32_t) xr_semantic_plan_call_target_count(semantic);
            for (uint32_t target_index = 0; target_index < target_count; target_index++) {
                const XrSemanticCallTargetRecord *target =
                    xr_semantic_plan_call_target(semantic, target_index);
                const XrSemanticOperationRecord *operation =
                    target ? xr_semantic_plan_operation(semantic, target->operation) : NULL;
                if (!target || !operation ||
                    !xr_target_program_function_is_reachable(out, module, operation->function))
                    continue;
                if (!reachability_mark_target(modules, module_count, module, target, operation, out,
                                              &changed))
                    goto invalid;
            }
            uint32_t operation_count = (uint32_t) xr_semantic_plan_operation_count(semantic);
            for (uint32_t operation_index = 0; operation_index < operation_count;
                 operation_index++) {
                const XrSemanticOperationRecord *operation =
                    xr_semantic_plan_operation(semantic, operation_index);
                if (!operation || operation->intrinsic_kind != XR_SEM_INTRINSIC_ARRAY_HOF ||
                    !xr_target_program_function_is_reachable(out, module, operation->function))
                    continue;
                if (!reachability_mark(out, module, operation->callable_function, &changed))
                    goto invalid;
            }
        }
    } while (changed);
    return true;

invalid:
    xr_target_program_reachability_dispose(out);
    return reachability_fail(error, error_size,
                             "program executable reachability edge is not exact");
}
