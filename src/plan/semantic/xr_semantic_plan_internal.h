/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_plan_internal.h - Mutable construction state for semantic plans
 */

#ifndef XR_SEMANTIC_PLAN_INTERNAL_H
#define XR_SEMANTIC_PLAN_INTERNAL_H

#include "xr_semantic_plan.h"
#include <stdatomic.h>

/* A shared binding that merely forwards another shared slot names the same
 * function, so naming a call target may follow that chain.  Builder and
 * verifier must stop at the same hop, or one of them would ground a target
 * the other cannot reproduce. */
#define XR_SEMANTIC_MAX_SHARED_CALLEE_HOPS 4u

typedef struct XrSemanticStringPool {
    char **items;
    uint32_t count;
    uint32_t capacity;
} XrSemanticStringPool;

struct XrSemanticPlan {
    atomic_uint_least32_t references;
    uint32_t schema;
    bool frozen;
    bool verified;
    bool module_set_verified;
    XrFingerprint fingerprint;
    XrFingerprint operation_registry_fingerprint;
    XrFingerprint stdlib_registry_fingerprint;
    XrSemanticTypeRecord *types;
    uint32_t type_count;
    uint32_t type_capacity;
    XrSemanticSourceClassRecord *source_classes;
    uint32_t source_class_count;
    uint32_t source_class_capacity;
    XrSemanticSourceMethodRecord *source_methods;
    uint32_t source_method_count;
    uint32_t source_method_capacity;
    XrSemanticFunctionRecord *functions;
    uint32_t function_count;
    uint32_t function_capacity;
    XrSemanticBlockRecord *blocks;
    uint32_t block_count;
    uint32_t block_capacity;
    XrSemanticOperationRecord *operations;
    uint32_t operation_count;
    uint32_t operation_capacity;
    XrSemanticCallTargetRecord *call_targets;
    uint32_t call_target_count;
    uint32_t call_target_capacity;
    XrSemanticDependencyRecord *dependencies;
    uint32_t dependency_count;
    uint32_t dependency_capacity;
    XrSemanticSourceExportRecord *source_exports;
    uint32_t source_export_count;
    uint32_t source_export_capacity;
    XrSemanticPlan **dependency_plans;
    uint32_t dependency_plan_count;
    uint32_t dependency_plan_capacity;
    XrSemanticEdgeRecord *edges;
    uint32_t edge_count;
    uint32_t edge_capacity;
    XrSemanticConstantRecord *constants;
    uint32_t constant_count;
    uint32_t constant_capacity;
    XrSemanticEntityRecord *entities;
    uint32_t entity_count;
    uint32_t entity_capacity;
    uint32_t *type_children;
    uint32_t type_child_count;
    uint32_t type_child_capacity;
    XrSemanticParameterRecord *parameters;
    uint32_t parameter_count;
    uint32_t parameter_capacity;
    XrSemanticCaptureRecord *captures;
    uint32_t capture_count;
    uint32_t capture_capacity;
    uint32_t *predecessors;
    uint32_t predecessor_count;
    uint32_t predecessor_capacity;
    XrSemanticOperandRecord *operands;
    uint32_t operand_count;
    uint32_t operand_capacity;
    const char **metadata;
    uint32_t metadata_count;
    uint32_t metadata_capacity;
    XrSemanticStringPool strings;
    XrOwnershipCertificate *ownership;
    XrSemanticProgramProvenance program_provenance;
    XrSemanticProgramFunctionBinding *program_function_bindings;
    uint32_t program_function_binding_count;
    XrSemanticProgramCallBinding *program_call_bindings;
    uint32_t program_call_binding_count;
};

XR_FUNC XrSemanticPlan *xr_semantic_plan_create(void);
XR_FUNC char *xr_semantic_plan_copy_string(XrSemanticPlan *plan, const char *text);
XR_FUNC bool xr_semantic_plan_verify_identity_set(const XrSemanticPlan *plan, char *error,
                                                  size_t error_size);
XR_FUNC void xr_semantic_plan_compute_fingerprint(const XrSemanticPlan *plan, XrFingerprint *out);
XR_FUNC bool xr_semantic_plan_freeze(XrSemanticPlan *plan, char *error, size_t error_size);
XR_FUNC void xr_semantic_plan_set_ownership(XrSemanticPlan *plan,
                                            XrOwnershipCertificate *ownership);
XR_FUNC bool xr_semantic_plan_set_program_provenance(
    XrSemanticPlan *plan,
    const XrSemanticProgramProvenance *provenance,
    const XrSemanticProgramFunctionBinding *function_bindings,
    uint32_t function_binding_count,
    const XrSemanticProgramCallBinding *call_bindings,
    uint32_t call_binding_count);

#endif  // XR_SEMANTIC_PLAN_INTERNAL_H
