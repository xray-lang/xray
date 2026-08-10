/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_semantic_verify.c - Independent SemanticPlan verifier
 */

#include "xr_semantic_verify.h"
#include "xr_semantic_plan_internal.h"
#include "../ownership/xr_ownership_check.h"
#include "../../base/xmalloc.h"
#include "../../ir/xi.h"
#include "../../ir/xi_own.h"
#include "../../ir/xi_ops_gen.h"
#include "../../runtime/value/xtype.h"
#include <stdio.h>
#include <string.h>

static bool report(char *error, size_t size, const char *code, const char *detail) {
    if (error && size)
        snprintf(error, size, "%s: %s", code, detail);
    return false;
}

static bool range_valid(uint32_t begin, uint32_t count, uint32_t limit) {
    return begin <= limit && count <= limit - begin;
}

static bool verify_id(const char *key, XrStableId actual) {
    XrStableId expected;
    XrFingerprint digest;
    return key && xr_stable_id_from_key(key, &expected, &digest) &&
           xr_stable_id_equal(expected, actual);
}

static bool verify_types(const XrSemanticPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->type_count; i++) {
        const XrSemanticTypeRecord *type = &plan->types[i];
        if (!verify_id(type->canonical_key, type->id))
            return report(error, error_size, "XR_SEM_0002", "type stable identity is invalid");
        if (type->kind >= XR_KIND_COUNT)
            return report(error, error_size, "XR_SEM_0005", "plan contains an invalid type kind");
        if (!range_valid(type->child_begin, type->child_count, plan->type_child_count))
            return report(error, error_size, "XR_SEM_0012", "type child range is invalid");
        for (uint16_t c = 0; c < type->child_count; c++) {
            if (plan->type_children[type->child_begin + c] >= plan->type_count)
                return report(error, error_size, "XR_SEM_0012", "type child index is invalid");
        }
    }
    return true;
}

static bool verify_functions(const XrSemanticPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *function = &plan->functions[i];
        if (!verify_id(function->canonical_key, function->id) || !function->name)
            return report(error, error_size, "XR_SEM_0002", "function identity is invalid");
        if (function->return_type >= plan->type_count ||
            !range_valid(function->parameter_begin, function->parameter_count,
                         plan->parameter_count) ||
            !range_valid(function->block_begin, function->block_count, plan->block_count))
            return report(error, error_size, "XR_SEM_0013", "function table range is invalid");
        if ((plan->types[function->return_type].flags & 16u) != 0 &&
            (function->return_provenance == XR_SEM_RETURN_NONE ||
             function->return_provenance > XR_SEM_RETURN_BORROWED_STATIC))
            return report(error, error_size, "XR_OWN_3000",
                          "reference-capable return has unknown provenance");
        for (uint16_t p = 0; p < function->parameter_count; p++) {
            if (plan->parameters[function->parameter_begin + p] >= plan->type_count)
                return report(error, error_size, "XR_SEM_0013", "parameter type is invalid");
        }
    }
    return true;
}

static bool predecessor_contains(const XrSemanticPlan *plan, uint32_t block, uint32_t predecessor) {
    const XrSemanticBlockRecord *record = &plan->blocks[block];
    for (uint16_t i = 0; i < record->predecessor_count; i++) {
        if (plan->predecessors[record->predecessor_begin + i] == predecessor)
            return true;
    }
    return false;
}

static bool verify_blocks(const XrSemanticPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->block_count; i++) {
        const XrSemanticBlockRecord *block = &plan->blocks[i];
        if (!verify_id(block->canonical_key, block->id) ||
            block->function >= plan->function_count || block->kind > XI_BLOCK_UNREACHABLE ||
            !range_valid(block->operation_begin, block->operation_count, plan->operation_count) ||
            !range_valid(block->predecessor_begin, block->predecessor_count,
                         plan->predecessor_count))
            return report(error, error_size, "XR_SEM_0014", "block record is invalid");
        for (unsigned s = 0; s < 2; s++) {
            uint32_t successor = block->successors[s];
            if (successor == XR_SEMANTIC_INDEX_NONE)
                continue;
            if (successor >= plan->block_count ||
                plan->blocks[successor].function != block->function ||
                !predecessor_contains(plan, successor, i))
                return report(error, error_size, "XR_SEM_0014", "CFG edge is not symmetric");
        }
    }
    return true;
}

static bool verify_operations(const XrSemanticPlan *plan, char *error, size_t error_size) {
    uint32_t value_count = 0;
    for (uint32_t f = 0; f < plan->function_count; f++) {
        uint64_t end = (uint64_t) plan->functions[f].value_begin + plan->functions[f].value_count;
        if (end > UINT32_MAX)
            return report(error, error_size, "XR_EXEC_5003", "SSA value index budget exhausted");
        if (end > value_count)
            value_count = (uint32_t) end;
    }
    uint8_t *defined = (uint8_t *) xr_calloc(value_count, sizeof(*defined));
    if (value_count && !defined)
        return report(error, error_size, "XR_EXEC_5003", "SSA verifier budget exhausted");
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        if (!verify_id(operation->canonical_key, operation->id) ||
            operation->function >= plan->function_count || operation->block >= plan->block_count ||
            plan->blocks[operation->block].function != operation->function ||
            operation->result_type >= plan->type_count || operation->result_value >= value_count ||
            operation->opcode >= XI_OP_COUNT || !xi_generated_op_name(operation->opcode) ||
            operation->effects != xi_generated_op_effects(operation->opcode) ||
            operation->ownership_use != xi_generated_op_own_use(operation->opcode) ||
            operation->result_ownership >= XI_GEN_RESULT_OWNERSHIP__COUNT ||
            (xi_generated_op_result_ownership(operation->opcode) == XI_GEN_RESULT_OWNERSHIP_NONE &&
             operation->result_ownership != XI_GEN_RESULT_OWNERSHIP_NONE) ||
            operation->parameter_ownership > XI_OWN_BORROWED ||
            operation->result_alias_operand < -1 ||
            (operation->result_alias_operand >= 0 &&
             (uint16_t) operation->result_alias_operand >= operation->operand_count) ||
            !range_valid(operation->operand_begin, operation->operand_count, plan->operand_count) ||
            !range_valid(operation->metadata_begin, operation->metadata_count,
                         plan->metadata_count) ||
            (operation->constant != XR_SEMANTIC_INDEX_NONE &&
             operation->constant >= plan->constant_count)) {
            xr_free(defined);
            return report(error, error_size, "XR_SEM_0015", "operation record is invalid");
        }
        if (defined[operation->result_value]) {
            xr_free(defined);
            return report(error, error_size, "XR_SEM_0015", "SSA value is defined twice");
        }
        defined[operation->result_value] = 1;
        if (operation->allocation_key &&
            !verify_id(operation->allocation_key, operation->allocation_id)) {
            xr_free(defined);
            return report(error, error_size, "XR_SEM_0002", "allocation identity is invalid");
        }
    }
    for (uint32_t i = 0; i < plan->operand_count; i++) {
        if (plan->operands[i] >= value_count || !defined[plan->operands[i]] ||
            plan->operand_ownership_actions[i] > XR_SEM_OPERAND_CONSUME) {
            xr_free(defined);
            return report(error, error_size, "XR_SEM_0015", "operand has no SSA definition");
        }
    }
    xr_free(defined);
    return true;
}

bool xr_semantic_plan_verify(const XrSemanticPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || plan->schema != XR_SEMANTIC_SCHEMA_VERSION)
        return report(error, error_size, "XR_SEM_0004",
                      "verifier requires a frozen exact-version SemanticPlan");
    if (plan->type_count > 1000000u || plan->function_count > 100000u ||
        plan->block_count > 2000000u || plan->operation_count > 10000000u ||
        plan->operand_count > 40000000u)
        return report(error, error_size, "XR_EXEC_5003", "SemanticPlan exceeds hard budgets");
    if (!verify_types(plan, error, error_size) || !verify_functions(plan, error, error_size) ||
        !verify_blocks(plan, error, error_size) || !verify_operations(plan, error, error_size) ||
        !xr_ownership_certificate_check(plan, error, error_size))
        return false;
    return true;
}
