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
#include "../semantic/xr_semantic_imported_static_method_shape.h"
#include "../semantic/xr_semantic_class_seal_shape.h"
#include "../semantic/xr_semantic_dependency_method_shape.h"
#include "../semantic/xr_semantic_local_call_target_shape.h"
#include "../semantic/xr_semantic_direct_callee_shape.h"
#include "../semantic/xr_semantic_allocation_shape.h"
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
    xr_free(reachability->module_identities);
    xr_free(reachability->module_fingerprints);
    xr_free(reachability->function_begins);
    xr_free(reachability->functions);
    xr_free(reachability->class_begins);
    xr_free(reachability->sealed_classes);
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

bool xr_target_program_class_is_sealed(const XrTargetProgramReachability *reachability,
                                       uint32_t module, uint32_t source_class) {
    if (!reachability || !reachability->class_begins || !reachability->sealed_classes ||
        module >= reachability->module_count)
        return false;
    uint32_t begin = reachability->class_begins[module];
    uint32_t end = reachability->class_begins[module + 1u];
    return source_class < end - begin && reachability->sealed_classes[begin + source_class] != 0;
}

static bool reachability_module_is_exact(const XrTargetProgramReachability *proof,
                                          uint32_t module, const XrSemanticPlan *semantic) {
    const XrSemanticEntityRecord *entity =
        semantic ? xr_semantic_plan_unique_module_entity(semantic) : NULL;
    return proof && entity && proof->module_identities && proof->module_fingerprints &&
           module < proof->module_count &&
           xr_stable_id_equal(proof->module_identities[module], entity->id) &&
           xr_fingerprint_equal(proof->module_fingerprints[module],
                                 xr_semantic_plan_fingerprint(semantic));
}

const XrSemanticSourceMethodRecord *xr_target_program_direct_dependency_method(
    const XrTargetProgramReachability *proof, uint32_t caller_module,
    const XrSemanticPlan *semantic, const XrSemanticCallTargetRecord *target,
    const XrSemanticPlan *dependency) {
    if (!reachability_module_is_exact(proof, caller_module, semantic))
        return NULL;
    const XrSemanticSourceMethodRecord *method =
        xr_semantic_dependency_method_is_exact(semantic, target, dependency);
    if (!method)
        return NULL;
    uint32_t match = UINT32_MAX;
    for (uint32_t module = 0; module < proof->module_count; module++) {
        if (!reachability_module_is_exact(proof, module, dependency))
            continue;
        if (match != UINT32_MAX)
            return NULL;
        match = module;
    }
    const XrSemanticSourceClassRecord *source_class =
        xr_semantic_plan_source_class(dependency, method->source_class);
    return match != UINT32_MAX && source_class &&
           ((source_class->flags & XR_SEM_SOURCE_CLASS_EXPLICIT_FINAL) != 0 ||
            xr_target_program_class_is_sealed(proof, match, method->source_class))
               ? method : NULL;
}

bool xr_target_program_call_binds_instance_method(
    const XrTargetProgramReachability *reachability, uint32_t module,
    const XrSemanticPlan *semantic, const XrSemanticCallTargetRecord *target) {
    if (!target || !reachability_module_is_exact(reachability, module, semantic))
        return false;
    if (target->kind != XR_SEM_CALL_TARGET_SOURCE_INSTANCE_METHOD_SEALED_CANDIDATE)
        return xr_semantic_call_target_binds_instance_method(target, semantic, NULL, 0);
    const XrSemanticTypeRecord *receiver = xr_semantic_plan_type(semantic, target->callable_type);
    return receiver && xr_target_program_class_is_sealed(reachability, module,
                                                        receiver->source_class);
}

static bool reachability_build_class_seals(const XrSemanticPlan *const *modules,
                                           uint32_t module_count,
                                           XrTargetProgramReachability *out) {
    out->module_identities = (XrStableId *) xr_calloc(module_count, sizeof(XrStableId));
    out->module_fingerprints = (XrFingerprint *) xr_calloc(module_count, sizeof(XrFingerprint));
    if (!out->module_identities || !out->module_fingerprints)
        return false;
    for (uint32_t m = 0; m < module_count; m++) {
        const XrSemanticEntityRecord *entity = xr_semantic_plan_unique_module_entity(modules[m]);
        if (!entity)
            return false;
        out->module_identities[m] = entity->id;
        out->module_fingerprints[m] = xr_semantic_plan_fingerprint(modules[m]);
    }
    out->class_begins = (uint32_t *) xr_calloc((size_t) module_count + 1u,
                                               sizeof(*out->class_begins));
    if (!out->class_begins)
        return false;
    for (uint32_t m = 0; m < module_count; m++) {
        size_t count = xr_semantic_plan_source_class_count(modules[m]);
        if (count > UINT32_MAX - out->class_begins[m])
            return false;
        out->class_begins[m + 1u] = out->class_begins[m] + (uint32_t) count;
    }
    uint32_t count = out->class_begins[module_count];
    out->sealed_classes = (uint8_t *) xr_calloc(count ? count : 1u, 1u);
    if (!out->sealed_classes)
        return false;
    for (uint32_t m = 0; m < module_count; m++) {
        uint32_t begin = out->class_begins[m];
        uint32_t end = out->class_begins[m + 1u];
        for (uint32_t c = 0; c < end - begin; c++) {
            const XrSemanticSourceClassRecord *source_class =
                xr_semantic_plan_source_class(modules[m], c);
            out->sealed_classes[begin + c] = source_class &&
                xr_semantic_graph_seals_class(modules[0], modules + 1u, module_count - 1u,
                                               source_class->name);
        }
    }
    return true;
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

    if (target->kind == XR_SEM_CALL_TARGET_SOURCE_EXPORT ||
        target->kind == XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_DEPENDENCY ||
        target->kind == XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY) {
        uint32_t dependency_module = UINT32_MAX;
        if (!reachability_dependency_module(modules, module_count, semantic, target->dependency,
                                            &dependency_module))
            return false;
        const XrSemanticPlan *dependency = modules[dependency_module];
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_STATIC_METHOD_DEPENDENCY) {
            uint32_t function = xr_semantic_dependency_static_method_is_exact(semantic, target, dependency);
            return function != XR_SEMANTIC_INDEX_NONE &&
                   reachability_mark(reachability, dependency_module, function, changed);
        }
        if (target->kind == XR_SEM_CALL_TARGET_SOURCE_METHOD_DEPENDENCY) {
            /* A dependency method names its declaration, not an export-table
             * ordinal. Retaining its body does not prove a closed dispatch
             * domain; the execution adapter must still establish that. */
            const XrSemanticSourceMethodRecord *method =
                xr_semantic_dependency_method_is_exact(semantic, target, dependency);
            return method && reachability_mark(reachability, dependency_module,
                                               method->function, changed);
        }
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

/* Retaining a body is independent of admitting its physical spawn ABI. The
 * storage verifier still proves initialization, dominance and argument transfer. */
static const XrSemanticOperationRecord *reachability_value_definition(
    const XrSemanticPlan *semantic, uint32_t value) {
    const XrSemanticOperationRecord *match = NULL;
    if (value == XR_SEMANTIC_INDEX_NONE)
        return NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
        const XrSemanticOperationRecord *operation = xr_semantic_plan_operation(semantic, i);
        if (!operation || operation->result_value != value)
            continue;
        if (match)
            return NULL;
        match = operation;
    }
    return match;
}

static uint32_t reachability_spawn_target(const XrSemanticPlan *semantic,
                                          const XrSemanticOperationRecord *spawn) {
    uint32_t operand_count = 0;
    const XrSemanticOperandRecord *operands = xr_semantic_plan_operands(semantic, &operand_count);
    if (!spawn || spawn->opcode != XI_GO || !operands || !spawn->operand_count ||
        spawn->operand_begin > operand_count ||
        spawn->operand_count > operand_count - spawn->operand_begin)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperandRecord *operand = &operands[spawn->operand_begin];
    const XrSemanticOperationRecord *source =
        reachability_value_definition(semantic, operand->value);
    if (!source || source->function != spawn->function || source->result_type != operand->type)
        return XR_SEMANTIC_INDEX_NONE;
    const XrSemanticOperationRecord *producer = source;
    const XrSemanticOperationRecord *store = NULL;
    if (source->opcode == XI_GET_SHARED) {
        for (uint32_t i = 0; i < xr_semantic_plan_operation_count(semantic); i++) {
            const XrSemanticOperationRecord *candidate = xr_semantic_plan_operation(semantic, i);
            if (!candidate || candidate->opcode != XI_SET_SHARED ||
                candidate->semantic_immediate != source->semantic_immediate)
                continue;
            if (store)
                return XR_SEMANTIC_INDEX_NONE;
            store = candidate;
        }
        if (!store || store->operand_count != 1 || store->operand_begin >= operand_count)
            return XR_SEMANTIC_INDEX_NONE;
        producer = reachability_value_definition(semantic, operands[store->operand_begin].value);
        if (!producer || producer->function != store->function ||
            producer->result_type != operands[store->operand_begin].type)
            return XR_SEMANTIC_INDEX_NONE;
    }
    const XrSemanticFunctionRecord *callee =
        xr_semantic_plan_function(semantic, producer->callable_function);
    if (producer->opcode != XI_CLOSURE_NEW || producer->operand_count != 0 ||
        !xr_semantic_allocation_identity_is_canonical(producer) ||
        producer->result_ownership != XI_GEN_RESULT_OWNERSHIP_OWNED || !callee ||
        callee->capture_count != 0 || callee->parent != producer->function ||
        (uint32_t) callee->parameter_count + 1u != spawn->operand_count ||
        (store && !xr_semantic_direct_local_callee_type_is_exact(
                      semantic, source, producer->callable_function)))
        return XR_SEMANTIC_INDEX_NONE;
    return producer->callable_function;
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

    if (!reachability_build_class_seals(modules, module_count, out)) {
        xr_target_program_reachability_dispose(out);
        return reachability_fail(error, error_size, "program class seal allocation is incomplete");
    }

    bool changed = false;
    for (uint32_t module = 0; module < module_count; module++) {
        uint32_t count = (uint32_t) xr_semantic_plan_function_count(modules[module]);
        for (uint32_t function = 0; function < count; function++) {
            const XrSemanticFunctionRecord *record =
                xr_semantic_plan_function(modules[module], function);
            if (record && (record->is_module_initializer || record->is_external_entry) &&
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
                if (!operation ||
                    !xr_target_program_function_is_reachable(out, module, operation->function))
                    continue;
                if (operation->intrinsic_kind == XR_SEM_INTRINSIC_ARRAY_HOF) {
                    if (!reachability_mark(out, module, operation->callable_function, &changed))
                        goto invalid;
                } else if (operation->opcode == XI_GO) {
                    uint32_t callee = reachability_spawn_target(semantic, operation);
                    if (callee != XR_SEMANTIC_INDEX_NONE &&
                        !reachability_mark(out, module, callee, &changed))
                        goto invalid;
                }
            }
        }
    } while (changed);
    return true;

invalid:
    xr_target_program_reachability_dispose(out);
    return reachability_fail(error, error_size,
                             "program executable reachability edge is not exact");
}
