/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_ops.c - Canonical target-neutral operation registry
 */

#include "xr_semantic_ops.h"
#include "xr_semantic_ops_gen.h"
#include "../../base/xsha256.h"
#include "../../ir/xi.h"
#include <stdio.h>
#include <string.h>

#define XR_SEMANTIC_CONTRACT_ROW(                                                                  \
    ident, name, owner_value, class_value, arity_value, operand_count_value, result_count_value,   \
    operand_relation_value, result_relation_value, result_kind_value, result_ownership_value,      \
    speculation_value, vn_value, algebraic_value, alias_value, sync_value, escape_use_value,       \
    escape_allocation_value, ownership_use_value, effects_value, requirements_value,               \
    observable_value, negated_value)                                                               \
    [XI_##ident] = {                                                                               \
        .canonical_name = name,                                                                    \
        .operation_class = class_value,                                                            \
        .operand_relation = operand_relation_value,                                                \
        .result_relation = result_relation_value,                                                  \
        .result_kind = result_kind_value,                                                          \
        .speculation = speculation_value,                                                          \
        .value_numbering = vn_value,                                                               \
        .algebraic_traits = algebraic_value,                                                       \
        .alias_scope = alias_value,                                                                \
        .synchronization = sync_value,                                                             \
        .escape_use = escape_use_value,                                                            \
        .escape_allocation = escape_allocation_value,                                              \
        .requirements = requirements_value,                                                        \
        .observable_contract = observable_value,                                                   \
        .negated_operation = negated_value,                                                        \
        .effects = effects_value,                                                                  \
        .opcode = XI_##ident,                                                                      \
        .owner = owner_value,                                                                      \
        .arity = arity_value,                                                                      \
        .operand_count = operand_count_value,                                                      \
        .result_count = result_count_value,                                                        \
        .result_ownership = result_ownership_value,                                                \
        .ownership_use = ownership_use_value,                                                      \
    },

static const XrSemanticOpContract xr_semantic_contracts[] = {
    XR_SEMANTIC_OPERATION_CONTRACTS(XR_SEMANTIC_CONTRACT_ROW)};

#undef XR_SEMANTIC_CONTRACT_ROW

typedef char xr_semantic_contract_count_must_match_xi
    [sizeof(xr_semantic_contracts) / sizeof(xr_semantic_contracts[0]) == XI_OP_COUNT ? 1 : -1];

static void hash_u64(XrSHA256Context *ctx, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static void hash_string(XrSHA256Context *ctx, const char *value) {
    size_t length = value ? strlen(value) : 0;
    hash_u64(ctx, length);
    if (length)
        xr_sha256_update(ctx, (const uint8_t *) value, length);
}

size_t xr_semantic_op_contract_count(void) {
    return sizeof(xr_semantic_contracts) / sizeof(xr_semantic_contracts[0]);
}

const XrSemanticOpContract *xr_semantic_op_contract(uint16_t opcode) {
    if (opcode >= xr_semantic_op_contract_count() || xr_semantic_contracts[opcode].opcode != opcode)
        return NULL;
    return &xr_semantic_contracts[opcode];
}

const char *xr_semantic_op_owner_name(uint8_t owner) {
    static const char *const names[] = {
        "declarative-primitive",
        "shared-semantic-kernel",
        "capability-provider",
        "generated-specialization",
    };
    return owner < XR_SEM_OWNER_COUNT ? names[owner] : NULL;
}

void xr_semantic_op_registry_fingerprint(XrFingerprint *out) {
    static const uint8_t domain[] = "xray-semantic-operation-registry-v1\0";
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1);
    hash_u64(&ctx, xr_semantic_op_contract_count());
    for (size_t i = 0; i < xr_semantic_op_contract_count(); i++) {
        const XrSemanticOpContract *contract = &xr_semantic_contracts[i];
        hash_string(&ctx, contract->canonical_name);
        hash_string(&ctx, contract->operation_class);
        hash_string(&ctx, contract->operand_relation);
        hash_string(&ctx, contract->result_relation);
        hash_string(&ctx, contract->result_kind);
        hash_string(&ctx, contract->speculation);
        hash_string(&ctx, contract->value_numbering);
        hash_string(&ctx, contract->algebraic_traits);
        hash_string(&ctx, contract->alias_scope);
        hash_string(&ctx, contract->synchronization);
        hash_string(&ctx, contract->escape_use);
        hash_string(&ctx, contract->escape_allocation);
        hash_string(&ctx, contract->requirements);
        hash_string(&ctx, contract->observable_contract);
        hash_string(&ctx, contract->negated_operation);
        hash_u64(&ctx, contract->effects);
        hash_u64(&ctx, contract->opcode);
        hash_u64(&ctx, contract->owner);
        hash_u64(&ctx, contract->arity);
        hash_u64(&ctx, contract->operand_count);
        hash_u64(&ctx, contract->result_count);
        hash_u64(&ctx, contract->result_ownership);
        hash_u64(&ctx, contract->ownership_use);
    }
    xr_sha256_final(&ctx, out->bytes);
}

bool xr_semantic_op_registry_verify(char *error, size_t error_size) {
    uint32_t owners_seen = 0;
    if (xr_semantic_op_contract_count() != XI_OP_COUNT) {
        if (error && error_size)
            snprintf(error, error_size, "XR_SEM_0017: operation registry is incomplete");
        return false;
    }
    for (size_t i = 0; i < xr_semantic_op_contract_count(); i++) {
        const XrSemanticOpContract *contract = &xr_semantic_contracts[i];
        bool valid = contract->opcode == i && contract->canonical_name &&
                     strncmp(contract->canonical_name, "xi.", 3) == 0 &&
                     contract->canonical_name[3] != '\0' && contract->operation_class &&
                     contract->operand_relation && contract->result_relation &&
                     contract->result_kind && contract->speculation && contract->value_numbering &&
                     contract->algebraic_traits && contract->alias_scope &&
                     contract->synchronization && contract->escape_use &&
                     contract->escape_allocation && contract->requirements &&
                     contract->observable_contract && contract->negated_operation &&
                     contract->owner < XR_SEM_OWNER_COUNT &&
                     contract->result_ownership < XR_SEM_RESULT_OWNERSHIP_COUNT &&
                     contract->ownership_use < XR_SEM_OWN_USE_COUNT;
        if (!valid) {
            if (error && error_size)
                snprintf(error, error_size, "XR_SEM_0017: operation registry row %zu is invalid",
                         i);
            return false;
        }
        for (size_t previous = 0; previous < i; previous++) {
            if (strcmp(xr_semantic_contracts[previous].canonical_name, contract->canonical_name) ==
                0) {
                if (error && error_size)
                    snprintf(error, error_size,
                             "XR_SEM_0017: operation registry has duplicate name %s",
                             contract->canonical_name);
                return false;
            }
        }
        owners_seen |= UINT32_C(1) << contract->owner;
    }
    if (owners_seen != (UINT32_C(1) << XR_SEM_OWNER_COUNT) - 1u) {
        if (error && error_size)
            snprintf(error, error_size,
                     "XR_SEM_0017: operation registry has an empty owner category");
        return false;
    }
    return true;
}
