/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_ops.h - Canonical target-neutral operation registry
 */

#ifndef XR_SEMANTIC_OPS_H
#define XR_SEMANTIC_OPS_H

#include "xr_semantic_ids.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_SEMANTIC_OP_ARITY_VARIADIC UINT8_MAX

typedef enum XrSemanticOpOwner {
    XR_SEM_OWNER_DECLARATIVE_PRIMITIVE = 0,
    XR_SEM_OWNER_SHARED_SEMANTIC_KERNEL,
    XR_SEM_OWNER_CAPABILITY_PROVIDER,
    XR_SEM_OWNER_GENERATED_SPECIALIZATION,
    XR_SEM_OWNER_COUNT,
} XrSemanticOpOwner;

typedef enum XrSemanticResultOwnership {
    XR_SEM_RESULT_OWNERSHIP_OWNED = 0,
    XR_SEM_RESULT_OWNERSHIP_BORROWED,
    XR_SEM_RESULT_OWNERSHIP_NONE,
    XR_SEM_RESULT_OWNERSHIP_CALL_RESULT,
    XR_SEM_RESULT_OWNERSHIP_COUNT,
} XrSemanticResultOwnership;

typedef enum XrSemanticOwnUse {
    XR_SEM_OWN_USE_CONSUME = 0,
    XR_SEM_OWN_USE_BORROW,
    XR_SEM_OWN_USE_STORED_VALUE,
    XR_SEM_OWN_USE_METHOD_ARGS,
    XR_SEM_OWN_USE_PASS,
    XR_SEM_OWN_USE_COUNT,
} XrSemanticOwnUse;

typedef struct XrSemanticOpContract {
    const char *canonical_name;
    const char *canonical_owner;
    const char *operation_class;
    const char *operand_relation;
    const char *result_relation;
    const char *result_kind;
    const char *speculation;
    const char *value_numbering;
    const char *algebraic_traits;
    const char *alias_scope;
    const char *synchronization;
    const char *escape_use;
    const char *escape_allocation;
    const char *requirements;
    const char *observable_contract;
    const char *negated_operation;
    uint64_t operation_id_hi;
    uint64_t operation_id_lo;
    uint64_t owner_id_hi;
    uint64_t owner_id_lo;
    uint32_t effects;
    uint16_t opcode;
    uint8_t owner;
    uint8_t arity;
    uint8_t operand_count;
    uint8_t result_count;
    uint8_t result_ownership;
    uint8_t ownership_use;
} XrSemanticOpContract;

XR_FUNC size_t xr_semantic_op_contract_count(void);
XR_FUNC const XrSemanticOpContract *xr_semantic_op_contract(uint16_t opcode);
XR_FUNC const char *xr_semantic_op_owner_name(uint8_t owner);
XR_FUNC void xr_semantic_op_registry_fingerprint(XrFingerprint *out);
XR_FUNC bool xr_semantic_op_registry_verify(char *error, size_t error_size);

#endif  // XR_SEMANTIC_OPS_H
