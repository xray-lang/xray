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
    XrFingerprint fingerprint;
    XrFingerprint operation_registry_fingerprint;
    XrSemanticTypeRecord *types;
    uint32_t type_count;
    uint32_t type_capacity;
    XrSemanticFunctionRecord *functions;
    uint32_t function_count;
    uint32_t function_capacity;
    XrSemanticBlockRecord *blocks;
    uint32_t block_count;
    uint32_t block_capacity;
    XrSemanticOperationRecord *operations;
    uint32_t operation_count;
    uint32_t operation_capacity;
    XrSemanticEdgeRecord *edges;
    uint32_t edge_count;
    uint32_t edge_capacity;
    XrSemanticConstantRecord *constants;
    uint32_t constant_count;
    uint32_t constant_capacity;
    uint32_t *type_children;
    uint32_t type_child_count;
    uint32_t type_child_capacity;
    uint32_t *parameters;
    uint32_t parameter_count;
    uint32_t parameter_capacity;
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
};

XR_FUNC XrSemanticPlan *xr_semantic_plan_create(void);
XR_FUNC char *xr_semantic_plan_copy_string(XrSemanticPlan *plan, const char *text);
XR_FUNC bool xr_semantic_plan_verify_identity_set(const XrSemanticPlan *plan, char *error,
                                                  size_t error_size);
XR_FUNC void xr_semantic_plan_compute_fingerprint(const XrSemanticPlan *plan, XrFingerprint *out);
XR_FUNC bool xr_semantic_plan_freeze(XrSemanticPlan *plan, char *error, size_t error_size);
XR_FUNC void xr_semantic_plan_set_ownership(XrSemanticPlan *plan,
                                            XrOwnershipCertificate *ownership);

#endif  // XR_SEMANTIC_PLAN_INTERNAL_H
