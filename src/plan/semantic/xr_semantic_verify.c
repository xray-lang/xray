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
#include "xr_semantic_graph.h"
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
        if ((type->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0 &&
            (type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0)
            return report(error, error_size, "XR_OWN_3000",
                          "ownership-root type is not reference-capable");
        if ((type->flags & XR_SEM_TYPE_BORROW_VIEW) != 0 &&
            ((type->flags & XR_SEM_TYPE_REFERENCE_CAPABLE) == 0 ||
             (type->flags & XR_SEM_TYPE_OWNERSHIP_ROOT) != 0))
            return report(error, error_size, "XR_OWN_3000",
                          "borrow-view type has an invalid ownership class");
        if (!range_valid(type->child_begin, type->child_count, plan->type_child_count))
            return report(error, error_size, "XR_SEM_0012", "type child range is invalid");
        for (uint16_t c = 0; c < type->child_count; c++) {
            if (plan->type_children[type->child_begin + c] >= plan->type_count)
                return report(error, error_size, "XR_SEM_0012", "type child index is invalid");
        }
        if (i > 0 && xr_stable_id_compare(plan->types[i - 1].id, type->id) >= 0)
            return report(error, error_size, "XR_SEM_0012",
                          "type table is not in strict stable-identity order");
    }
    return true;
}

static bool verify_functions(const XrSemanticPlan *plan, char *error, size_t error_size) {
    uint32_t parameter_cursor = 0;
    uint32_t block_cursor = 0;
    uint32_t value_cursor = 0;
    for (uint32_t i = 0; i < plan->function_count; i++) {
        const XrSemanticFunctionRecord *function = &plan->functions[i];
        if (!verify_id(function->canonical_key, function->id) || !function->name)
            return report(error, error_size, "XR_SEM_0002", "function identity is invalid");
        if (function->return_type >= plan->type_count ||
            !range_valid(function->parameter_begin, function->parameter_count,
                         plan->parameter_count) ||
            !range_valid(function->block_begin, function->block_count, plan->block_count) ||
            function->parameter_begin != parameter_cursor ||
            function->block_begin != block_cursor || function->value_begin != value_cursor)
            return report(error, error_size, "XR_SEM_0013", "function table range is invalid");
        if ((plan->types[function->return_type].flags & XR_SEM_TYPE_REFERENCE_CAPABLE) != 0 &&
            (function->return_provenance == XR_SEM_RETURN_NONE ||
             function->return_provenance > XR_SEM_RETURN_BORROWED_STATIC))
            return report(error, error_size, "XR_OWN_3000",
                          "reference-capable return has unknown provenance");
        for (uint16_t p = 0; p < function->parameter_count; p++) {
            if (plan->parameters[function->parameter_begin + p] >= plan->type_count)
                return report(error, error_size, "XR_SEM_0013", "parameter type is invalid");
        }
        if (UINT32_MAX - parameter_cursor < function->parameter_count ||
            UINT32_MAX - block_cursor < function->block_count ||
            UINT32_MAX - value_cursor < function->value_count)
            return report(error, error_size, "XR_EXEC_5003", "function index budget exhausted");
        parameter_cursor += function->parameter_count;
        block_cursor += function->block_count;
        value_cursor += function->value_count;
    }
    if (parameter_cursor != plan->parameter_count || block_cursor != plan->block_count)
        return report(error, error_size, "XR_SEM_0013",
                      "function table does not exactly partition parameter and block tables");
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

#define XR_SEM_BLOCK_EDGE_SUCCESSOR_0 (1u << 0)
#define XR_SEM_BLOCK_EDGE_SUCCESSOR_1 (1u << 1)
#define XR_SEM_OPERATION_EDGE_ERROR (1u << 0)
#define XR_SEM_OPERATION_EDGE_PANIC (1u << 1)

static bool verify_blocks(const XrSemanticPlan *plan, const uint8_t *edge_mask, char *error,
                          size_t error_size) {
    uint32_t operation_cursor = 0;
    uint32_t predecessor_cursor = 0;
    for (uint32_t i = 0; i < plan->block_count; i++) {
        const XrSemanticBlockRecord *block = &plan->blocks[i];
        if (!verify_id(block->canonical_key, block->id) ||
            block->function >= plan->function_count || block->kind > XI_BLOCK_UNREACHABLE ||
            !range_valid(block->operation_begin, block->operation_count, plan->operation_count) ||
            !range_valid(block->predecessor_begin, block->predecessor_count,
                         plan->predecessor_count) ||
            block->operation_begin != operation_cursor ||
            block->predecessor_begin != predecessor_cursor)
            return report(error, error_size, "XR_SEM_0014", "block record is invalid");
        for (unsigned s = 0; s < 2; s++) {
            uint32_t successor = block->successors[s];
            if (successor == XR_SEMANTIC_INDEX_NONE)
                continue;
            if (successor >= plan->block_count ||
                plan->blocks[successor].function != block->function ||
                !predecessor_contains(plan, successor, i) ||
                (edge_mask[i] & (uint8_t) (1u << s)) == 0)
                return report(error, error_size, "XR_SEM_0014", "CFG edge is not symmetric");
        }
        for (uint16_t p = 0; p < block->predecessor_count; p++) {
            uint32_t predecessor = plan->predecessors[block->predecessor_begin + p];
            if (predecessor >= plan->block_count ||
                plan->blocks[predecessor].function != block->function) {
                if (error && error_size)
                    snprintf(error, error_size,
                             "XR_SEM_0014: SSA predecessor belongs to another function "
                             "(function=%u block=%u predecessor=%u)",
                             block->function, i, predecessor);
                return false;
            }
        }
        if (UINT32_MAX - operation_cursor < block->operation_count ||
            UINT32_MAX - predecessor_cursor < block->predecessor_count)
            return report(error, error_size, "XR_EXEC_5003", "block index budget exhausted");
        operation_cursor += block->operation_count;
        predecessor_cursor += block->predecessor_count;
    }
    if (operation_cursor != plan->operation_count || predecessor_cursor != plan->predecessor_count)
        return report(error, error_size, "XR_SEM_0014",
                      "block table does not exactly partition operation and predecessor tables");
    return true;
}

static bool verify_edges(const XrSemanticPlan *plan, uint8_t *block_edge_mask,
                         uint8_t *operation_edge_mask, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        const XrSemanticEdgeRecord *edge = &plan->edges[i];
        if (!verify_id(edge->canonical_key, edge->id) || edge->function >= plan->function_count ||
            edge->from_block >= plan->block_count || edge->to_block >= plan->block_count ||
            edge->kind > XR_SEM_EDGE_RESUME ||
            plan->blocks[edge->from_block].function != edge->function ||
            plan->blocks[edge->to_block].function != edge->function ||
            ((edge->kind == XR_SEM_EDGE_NORMAL || edge->kind == XR_SEM_EDGE_ERROR) &&
             !predecessor_contains(plan, edge->to_block, edge->from_block)))
            return report(error, error_size, "XR_SEM_0010",
                          "semantic control-edge record is invalid");
        const XrSemanticBlockRecord *source = &plan->blocks[edge->from_block];
        if (edge->kind == XR_SEM_EDGE_NORMAL) {
            if (edge->flags != 0 || edge->operation != XR_SEMANTIC_INDEX_NONE ||
                (source->successors[0] != edge->to_block &&
                 source->successors[1] != edge->to_block))
                return report(error, error_size, "XR_SEM_0010",
                              "normal semantic edge disagrees with the canonical CFG");
        } else {
            if (edge->operation >= plan->operation_count ||
                plan->operations[edge->operation].function != edge->function ||
                plan->operations[edge->operation].block != edge->from_block)
                return report(error, error_size, "XR_SEM_0010",
                              "exceptional semantic edge has no valid source operation");
        }
        if (edge->kind == XR_SEM_EDGE_ERROR) {
            const XrSemanticOperationRecord *operation = &plan->operations[edge->operation];
            bool checked_branch = operation->opcode == XI_ERR_CHECK &&
                                  source->control_value == operation->result_value &&
                                  source->successors[0] == edge->to_block;
            bool explicit_error =
                operation->opcode == XI_ERR_SET && (source->successors[0] == edge->to_block ||
                                                    source->successors[1] == edge->to_block);
            if (edge->flags != 0 || (!checked_branch && !explicit_error))
                return report(error, error_size, "XR_SEM_0010",
                              "error edge is not backed by an explicit error-channel operation");
            if ((operation_edge_mask[edge->operation] & XR_SEM_OPERATION_EDGE_ERROR) != 0)
                return report(error, error_size, "XR_SEM_0010",
                              "operation has duplicate explicit error edges");
            operation_edge_mask[edge->operation] |= XR_SEM_OPERATION_EDGE_ERROR;
        }
        if (edge->kind == XR_SEM_EDGE_PANIC) {
            if (edge->flags != XR_SEM_EDGE_HANDLER_SCOPE ||
                plan->operations[edge->operation].opcode != XI_TRY ||
                plan->operations[edge->operation].evidence[7] != edge->to_block)
                return report(error, error_size, "XR_SEM_0010",
                              "panic edge is not backed by its try-handler operation");
            if ((operation_edge_mask[edge->operation] & XR_SEM_OPERATION_EDGE_PANIC) != 0)
                return report(error, error_size, "XR_SEM_0010",
                              "operation has duplicate explicit panic edges");
            operation_edge_mask[edge->operation] |= XR_SEM_OPERATION_EDGE_PANIC;
        }
        if (edge->kind != XR_SEM_EDGE_NORMAL && edge->kind != XR_SEM_EDGE_ERROR &&
            edge->kind != XR_SEM_EDGE_PANIC)
            return report(error, error_size, "XR_SEM_0010",
                          "semantic edge kind has no implemented contract");
        if (edge->kind == XR_SEM_EDGE_NORMAL || edge->kind == XR_SEM_EDGE_ERROR) {
            uint8_t mask = 0;
            if (source->successors[0] == edge->to_block)
                mask |= XR_SEM_BLOCK_EDGE_SUCCESSOR_0;
            if (source->successors[1] == edge->to_block)
                mask |= XR_SEM_BLOCK_EDGE_SUCCESSOR_1;
            if (mask == 0 || (block_edge_mask[edge->from_block] & mask) != 0)
                return report(error, error_size, "XR_SEM_0010",
                              "CFG successor has missing or duplicate semantic edges");
            block_edge_mask[edge->from_block] |= mask;
        }
    }
    return true;
}

static bool verify_ssa_use(const XrSemanticPlan *plan, const XrSemanticGraph *graph,
                           const uint32_t *definitions, uint32_t operation_index,
                           uint16_t operand_index, char *error, size_t error_size) {
    const XrSemanticOperationRecord *operation = &plan->operations[operation_index];
    uint32_t value = plan->operands[operation->operand_begin + operand_index];
    uint32_t definition_index = definitions[value];
    const XrSemanticOperationRecord *definition = &plan->operations[definition_index];
    if (definition->function != operation->function)
        return report(error, error_size, "XR_SEM_0016", "SSA use crosses a function boundary");

    if (operation->opcode == XI_PHI) {
        const XrSemanticBlockRecord *block = &plan->blocks[operation->block];
        if (operation->operand_count != block->predecessor_count)
            return report(error, error_size, "XR_SEM_0016",
                          "PHI operands do not match SSA predecessor slots");
        uint32_t predecessor = plan->predecessors[block->predecessor_begin + operand_index];
        if (!xr_semantic_graph_is_reachable(graph, operation->block))
            return true;
        /* Braun construction and panic lowering may retain an SSA-only
         * predecessor slot after the executable edge becomes unreachable.
         * Such an input can never be selected at runtime, so dominance is
         * defined only for reachable incoming edges.  The slot and operand
         * remain frozen for exact SSA reconstruction. */
        if (!xr_semantic_graph_is_reachable(graph, predecessor))
            return true;
        if (!xr_semantic_graph_dominates(graph, definition->block, predecessor))
            return report(error, error_size, "XR_SEM_0016",
                          "PHI input definition does not dominate its incoming edge");
        return true;
    }

    if (!xr_semantic_graph_is_reachable(graph, operation->block))
        return true;
    if (!xr_semantic_graph_dominates(graph, definition->block, operation->block))
        return report(error, error_size, "XR_SEM_0016", "SSA definition does not dominate its use");
    if (definition->block == operation->block && definition_index >= operation_index)
        return report(error, error_size, "XR_SEM_0016",
                      "same-block SSA definition does not precede its use");
    return true;
}

static bool verify_operations(const XrSemanticPlan *plan, const uint8_t *edge_mask,
                              const XrSemanticGraph *graph, char *error, size_t error_size) {
    uint32_t value_count = 0;
    for (uint32_t f = 0; f < plan->function_count; f++) {
        uint64_t end = (uint64_t) plan->functions[f].value_begin + plan->functions[f].value_count;
        if (end > UINT32_MAX)
            return report(error, error_size, "XR_EXEC_5003", "SSA value index budget exhausted");
        if (end > value_count)
            value_count = (uint32_t) end;
    }
    uint32_t *definitions = (uint32_t *) xr_malloc((size_t) value_count * sizeof(*definitions));
    if (value_count && !definitions)
        return report(error, error_size, "XR_EXEC_5003", "SSA verifier budget exhausted");
    for (uint32_t value = 0; value < value_count; value++)
        definitions[value] = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        const XrSemanticFunctionRecord *function = operation->function < plan->function_count
                                                       ? &plan->functions[operation->function]
                                                       : NULL;
        uint8_t arity = operation->opcode < XI_OP_COUNT ? xi_generated_op_arity(operation->opcode)
                                                        : XI_OP_ARITY_VARIADIC;
        if (!verify_id(operation->canonical_key, operation->id) || !function ||
            operation->block >= plan->block_count ||
            plan->blocks[operation->block].function != operation->function ||
            i < plan->blocks[operation->block].operation_begin ||
            i >= plan->blocks[operation->block].operation_begin +
                     plan->blocks[operation->block].operation_count ||
            operation->result_type >= plan->type_count || operation->result_value >= value_count ||
            operation->result_value < function->value_begin ||
            operation->result_value >= function->value_begin + function->value_count ||
            operation->opcode >= XI_OP_COUNT || !xi_generated_op_name(operation->opcode) ||
            (arity != XI_OP_ARITY_VARIADIC && arity != operation->operand_count) ||
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
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0015", "operation record is invalid");
        }
        if (definitions[operation->result_value] != XR_SEMANTIC_INDEX_NONE) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0015", "SSA value is defined twice");
        }
        definitions[operation->result_value] = i;
        if (operation->allocation_key &&
            !verify_id(operation->allocation_key, operation->allocation_id)) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0002", "allocation identity is invalid");
        }
        if (operation->opcode == XI_TRY && (operation->evidence[7] >= plan->block_count ||
                                            (edge_mask[i] & XR_SEM_OPERATION_EDGE_PANIC) == 0)) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0010",
                          "try operation is missing its explicit panic edge");
        }
        if (operation->opcode == XI_ERR_CHECK &&
            plan->blocks[operation->block].control_value == operation->result_value &&
            plan->blocks[operation->block].successors[0] != XR_SEMANTIC_INDEX_NONE &&
            (edge_mask[i] & XR_SEM_OPERATION_EDGE_ERROR) == 0) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0010",
                          "error check is missing its explicit error edge");
        }
    }
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        const XrSemanticOperationRecord *operation = &plan->operations[i];
        for (uint16_t operand = 0; operand < operation->operand_count; operand++) {
            uint32_t cursor = operation->operand_begin + operand;
            if (plan->operands[cursor] >= value_count ||
                definitions[plan->operands[cursor]] == XR_SEMANTIC_INDEX_NONE ||
                plan->operand_ownership_actions[cursor] > XR_SEM_OPERAND_CONSUME) {
                xr_free(definitions);
                return report(error, error_size, "XR_SEM_0015", "operand has no SSA definition");
            }
            if (!verify_ssa_use(plan, graph, definitions, i, operand, error, error_size)) {
                xr_free(definitions);
                return false;
            }
        }
    }
    for (uint32_t b = 0; b < plan->block_count; b++) {
        const XrSemanticBlockRecord *block = &plan->blocks[b];
        if (block->control_value == XR_SEMANTIC_INDEX_NONE)
            continue;
        if (block->control_value >= value_count ||
            definitions[block->control_value] == XR_SEMANTIC_INDEX_NONE) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0016",
                          "block control value has no SSA definition");
        }
        const XrSemanticOperationRecord *definition =
            &plan->operations[definitions[block->control_value]];
        if (definition->function != block->function ||
            (xr_semantic_graph_is_reachable(graph, b) &&
             !xr_semantic_graph_dominates(graph, definition->block, b))) {
            xr_free(definitions);
            return report(error, error_size, "XR_SEM_0016",
                          "block control definition does not dominate the terminator");
        }
    }
    xr_free(definitions);
    return true;
}

static bool verify_constants(const XrSemanticPlan *plan, char *error, size_t error_size) {
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        const XrSemanticConstantRecord *constant = &plan->constants[i];
        bool has_text = constant->string != NULL;
        if (constant->type >= plan->type_count || constant->kind <= XR_SEM_CONST_NONE ||
            constant->kind > XR_SEM_CONST_ENUM_NAMESPACE)
            return report(error, error_size, "XR_SEM_0009",
                          "constant kind or type is not exactly supported");
        if ((constant->kind == XR_SEM_CONST_STRING ||
             constant->kind == XR_SEM_CONST_ENUM_NAMESPACE) != has_text)
            return report(error, error_size, "XR_SEM_0009",
                          "constant string payload does not match its kind");
        if (constant->kind == XR_SEM_CONST_BOOL && constant->integer != 0 && constant->integer != 1)
            return report(error, error_size, "XR_SEM_0009",
                          "boolean constant is not canonically encoded");
        if (constant->kind == XR_SEM_CONST_RUNE &&
            (constant->integer < 0 || constant->integer > 0x10FFFF ||
             (constant->integer >= 0xD800 && constant->integer <= 0xDFFF)))
            return report(error, error_size, "XR_SEM_0009",
                          "rune constant is not a Unicode scalar value");
    }
    return true;
}

bool xr_semantic_plan_verify(const XrSemanticPlan *plan, char *error, size_t error_size) {
    if (!plan || !plan->frozen || plan->schema != XR_SEMANTIC_SCHEMA_VERSION)
        return report(error, error_size, "XR_SEM_0004",
                      "verifier requires a frozen exact-version SemanticPlan");
    if (plan->type_count > 1000000u || plan->function_count > 100000u ||
        plan->block_count > 2000000u || plan->operation_count > 10000000u ||
        plan->edge_count > 40000000u || plan->operand_count > 40000000u)
        return report(error, error_size, "XR_EXEC_5003", "SemanticPlan exceeds hard budgets");
    uint8_t *block_edge_mask = (uint8_t *) xr_calloc(plan->block_count, sizeof(*block_edge_mask));
    uint8_t *operation_edge_mask =
        (uint8_t *) xr_calloc(plan->operation_count, sizeof(*operation_edge_mask));
    if ((plan->block_count && !block_edge_mask) ||
        (plan->operation_count && !operation_edge_mask)) {
        xr_free(block_edge_mask);
        xr_free(operation_edge_mask);
        return report(error, error_size, "XR_EXEC_5003", "edge verifier budget exhausted");
    }
    XrSemanticGraph graph = {0};
    bool verified = xr_semantic_plan_verify_identity_set(plan, error, error_size) &&
                    verify_types(plan, error, error_size) &&
                    verify_functions(plan, error, error_size) &&
                    verify_edges(plan, block_edge_mask, operation_edge_mask, error, error_size) &&
                    verify_blocks(plan, block_edge_mask, error, error_size) &&
                    xr_semantic_graph_build(plan, &graph, error, error_size) &&
                    verify_operations(plan, operation_edge_mask, &graph, error, error_size) &&
                    verify_constants(plan, error, error_size) &&
                    xr_ownership_certificate_check(plan, error, error_size);
    xr_semantic_graph_dispose(&graph);
    xr_free(block_edge_mask);
    xr_free(operation_edge_mask);
    if (!verified)
        return false;
    XrFingerprint actual;
    xr_semantic_plan_compute_fingerprint(plan, &actual);
    if (!xr_fingerprint_equal(actual, plan->fingerprint))
        return report(error, error_size, "XR_SEM_0004",
                      "frozen SemanticPlan fingerprint changed after freeze");
    return true;
}
