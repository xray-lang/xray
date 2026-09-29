/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_verify.c - Bounded stage, type, CFG, and SSA validation
 *
 * KEY CONCEPT:
 *   Structural admission precedes graph traversal. Dominance is checked from
 *   control flow, never inferred from the order of blocks in storage.
 *   A protected block may fault before any of its instructions completes, so
 *   its panic edge leaves from the block's start: a handler sees only values
 *   that strictly dominate every block it protects.
 */

#include "xxir_internal.h"
#include "xxir_generic.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "xxir_initialization.h"
#include "../base/xmalloc.h"
#include <limits.h>

typedef enum ResultRule {
    RULE_UNIT, RULE_BOOL, RULE_I64, RULE_INTEGER, RULE_NUMBER, RULE_FLOAT, RULE_SCALAR, RULE_VALUE, RULE_OWNED, RULE_STRING, RULE_CALL, RULE_ATOMIC, RULE_ARRAY, RULE_NOMINAL, RULE_ROOT, RULE_PANIC
} ResultRule;
typedef struct OpRule {
    uint8_t stages;
    ResultRule result;
    uint8_t operands;
    uint8_t edges;
    bool terminal;
} OpRule;

static const OpRule op_rules[XR_XIR_OP_COUNT] = {
    {0, RULE_UNIT, 0, 0, false},
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) \
    {stages, RULE_##rule, operands, edges, terminal != 0},
#include "xxir_ops.def"
#undef XR_XIR_OP
};

typedef struct VerifyContext {
    XrXirBudget remaining;
    XrXirDiagnostic location;
    const XrXirModule *module;
} VerifyContext;

typedef struct Graph {
    uint32_t *owner;
    uint32_t *queue;
    uint32_t *head;
    uint32_t *fault_head;
    uint32_t *predecessor;
    uint32_t *next;
    uint8_t *reachable;
    uint64_t *dominators;
    uint64_t *meet;
    uint32_t words;
} Graph;

static bool spend(uint64_t *remaining, uint64_t amount) {
    if (amount > *remaining)
        return false;
    *remaining -= amount;
    return true;
}

static bool spend_count(uint32_t *remaining, uint32_t amount) {
    if (amount > *remaining)
        return false;
    *remaining -= amount;
    return true;
}

static bool scalar(XrXirType type) {
    return type == XR_XIR_BOOL || xr_xir_type_is_number(type);
}

static uint32_t operand_count(const XrXirFunction *function, const XrXirInstruction *op,
                              const XrXirModule *module) {
    (void) module;
    if (xr_xir_op_uses_operand_table(op->op)) return op->args[1];
    return op->op == XR_XIR_RETURN ? (function->result != XR_XIR_UNIT) : op_rules[op->op].operands;
}

static XrXirStatus declaration_instruction(const XrXirFunction *function,
                                           const XrXirInstruction *op, const XrXirModule *module) {
    const XrXirDeclarations *d = module->declarations;
    if (op->op == XR_XIR_CONST_STRING)
        return d && op->immediate >= 0 && (uint64_t) op->immediate < d->literal_count ?
            XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_ATOMIC_I64_NEW) return d ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (op->op != XR_XIR_SLOT_LOAD && op->op != XR_XIR_SLOT_INIT && op->op != XR_XIR_SLOT_STORE &&
        op->op != XR_XIR_SLOT_PLACE)
        return XR_XIR_OK;
    if (!d || op->immediate < 0 || (uint64_t) op->immediate >= d->slot_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSlot *slot = &d->slots[op->immediate];
    uint32_t caller = (uint32_t) (function - module->functions);
    if (d->functions[caller].module != slot->module) return XR_XIR_BAD_STRUCTURE;
    if ((op->op == XR_XIR_SLOT_LOAD || op->op == XR_XIR_SLOT_PLACE) && op->type != slot->type) return XR_XIR_BAD_TYPE;
    if (op->op == XR_XIR_SLOT_INIT && d->modules[slot->module].initializer != caller)
        return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_SLOT_STORE && (!slot->mutable || slot->module != d->root_module))
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus instruction_shape(const XrXirFunction *function,
                                    const XrXirInstruction *op, XrXirStage stage,
                                    const XrXirModule *module, XrXirBudget *remaining) {
    if (op->op <= XR_XIR_INVALID || op->op >= XR_XIR_OP_COUNT)
        return XR_XIR_BAD_STRUCTURE;
    const OpRule *rule = &op_rules[op->op];
    if (!(rule->stages & stage))
        return XR_XIR_BAD_STAGE;
    XrXirStatus declared = declaration_instruction(function, op, module);
    if (declared != XR_XIR_OK) return declared;
    bool range = xr_xir_op_uses_operand_table(op->op);
    if (range && (op->args[1] > 65536 || op->args[0] > function->operand_count ||
        op->args[1] > function->operand_count - op->args[0] || (!op->args[1] && op->args[0])))
        return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_PHI && (!op->args[1] || op->args[1] % 2)) return XR_XIR_BAD_STRUCTURE;
    if ((op->op == XR_XIR_ARRAY_SET || op->op == XR_XIR_STRING_INDEX_OF) && op->args[1] != 3) return XR_XIR_BAD_STRUCTURE;
    uint32_t caller_id = (uint32_t) (function - module->functions);
    if (op->op == XR_XIR_CELL_NEW && (!module->declarations || !xr_xir_type_is_cell(module->types, op->type))) return XR_XIR_BAD_TYPE;
    if (op->op == XR_XIR_CELL_READ && xr_xir_type_is_cell(module->types, op->type)) return XR_XIR_BAD_TYPE;
    if (xr_xir_op_references_function(op->op)) {
        if (op->immediate < 0 || (uint64_t) op->immediate >= module->function_count)
            return XR_XIR_BAD_STRUCTURE;
        bool registration = op->op == XR_XIR_CLEANUP_REGISTER;
        if ((registration && !module->declarations) || (module->declarations &&
            module->declarations->functions[op->immediate].cleanup_owner != (registration ? caller_id + 1 : 0)))
            return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *callee = &module->functions[op->immediate];
        if ((op->op != XR_XIR_FUNCTION_REF && callee->parameter_count != op->args[1]) || (callee->parameter_count && !callee->parameters))
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus generic_status = xr_xir_generic_call(module, caller_id, op, remaining);
        if (generic_status != XR_XIR_OK) return generic_status;
        if (registration && module->generics) {
            const XrXirGeneric *owner = &module->generics[caller_id];
            if (op->type_arguments[1] != owner->parameter_count) return XR_XIR_BAD_TYPE;
            for (uint32_t a = 0; a < owner->parameter_count; ++a) {
                if (!spend(&remaining->work, 1)) return XR_XIR_BUDGET;
                if (owner->arguments[op->type_arguments[0] + a] != (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE + a))
                    return XR_XIR_BAD_TYPE;
            }
        }
        XrXirType result = op->type;
        const XrXirTypeNode *signature = NULL;
        if (op->op == XR_XIR_FUNCTION_REF) {
            signature = xr_xir_callable_signature(module->types, op->type);
            if (!module->declarations || !signature || op->args[1] > callee->parameter_count ||
                signature->parameter_count != callee->parameter_count - op->args[1])
                return XR_XIR_BAD_TYPE;
            if ((signature->flags & XR_XIR_CALLABLE_NO_SUSPEND) &&
                !(module->declarations->functions[op->immediate].promises & XR_XIR_FUNCTION_NO_SUSPEND))
                return XR_XIR_BAD_TYPE;
            result = signature->result;
        }
        generic_status = xr_xir_call_type_matches(module, caller_id, op, callee->result, result, remaining);
        if (generic_status != XR_XIR_OK) return generic_status;
        if (signature) for (uint32_t p = 0; p < signature->parameter_count; ++p) {
            generic_status = xr_xir_call_type_matches(module, caller_id, op, callee->parameters[p + op->args[1]], signature->parameters[p].type, remaining);
            if (generic_status != XR_XIR_OK) return generic_status;
        }
        if (module->declarations) {
            const XrXirDeclarations *d = module->declarations;
            uint32_t caller_module = d->functions[caller_id].module;
            uint32_t callee_module = d->functions[op->immediate].module;
            if ((d->functions[op->immediate].member_access &&
                    d->functions[caller_id].nominal_owner != d->functions[op->immediate].nominal_owner) ||
                (uint32_t) op->immediate == d->modules[callee_module].initializer ||
                !xr_xir_module_imports(d, caller_module, callee_module) ||
                (caller_module != callee_module && !d->functions[op->immediate].exported))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
        if (op->immediate < 0 || (uint64_t) op->immediate >= (uint64_t) function->parameter_count + function->instruction_count)
            return XR_XIR_BAD_VALUE;
        XrXirType type = xr_xir_operand_type(function, (uint32_t) op->immediate);
        const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, type);
        if (!signature || signature->parameter_count != op->args[1] || signature->result != op->type)
            return XR_XIR_BAD_TYPE;
    }
    if ((rule->result == RULE_UNIT && op->type != XR_XIR_UNIT) ||
        (rule->result == RULE_BOOL && op->type != XR_XIR_BOOL) ||
        (rule->result == RULE_I64 && op->type != XR_XIR_I64) ||
        (rule->result == RULE_NUMBER && !xr_xir_type_is_number(op->type)) ||
        (rule->result == RULE_FLOAT && !xr_xir_float_bits(op->type)) ||
        (rule->result == RULE_INTEGER && !xr_xir_type_is_integer(op->type)) ||
        (rule->result == RULE_OWNED && (!xr_xir_type_is_owned(module->types, op->type) || !xr_xir_type_in_context(module, caller_id, op->type))) ||
        (rule->result == RULE_STRING && op->type != XR_XIR_STRING) ||
        (rule->result == RULE_PANIC && op->type != XR_XIR_UNIT && op->type != XR_XIR_PANIC_INFO) ||
        (rule->result == RULE_ATOMIC && op->type != XR_XIR_ATOMIC_I64) ||
        (rule->result == RULE_ARRAY && (!xr_xir_type_is_array(module->types, op->type) ||
            !xr_xir_type_in_context(module, caller_id, op->type))) ||
        (rule->result == RULE_ROOT && ((!xr_xir_type_is_array(module->types, op->type) &&
            !xr_xir_type_is_nominal(module->types, op->type)) || !xr_xir_type_in_context(module, caller_id, op->type))) ||
        (rule->result == RULE_NOMINAL && !xr_xir_type_is_nominal(module->types, op->type)) ||
        (rule->result == RULE_VALUE && !xr_xir_type_in_context(module, caller_id, op->type)) ||
        (rule->result == RULE_SCALAR && !scalar(op->type)))
        return XR_XIR_BAD_TYPE;
    uint32_t operands = operand_count(function, op, module);
    for (uint32_t i = range || op->op == XR_XIR_ENUM_GET ? 2 : operands; i < 2; ++i)
        if (op->args[i])
            return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < rule->edges; ++i)
        if (!op->targets[i] || op->targets[i] >= function->block_count)
            return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = rule->edges; i < 2; ++i)
        if (op->targets[i])
            return XR_XIR_BAD_STRUCTURE;
    if (!xr_xir_op_references_function(op->op) &&
        (op->type_arguments[0] || op->type_arguments[1])) return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_CONST_BOOL || op->op == XR_XIR_LOCAL_UNINIT) {
        if (op->immediate != 0 && op->immediate != 1)
            return XR_XIR_BAD_TYPE;
    } else if (op->op == XR_XIR_CONST_INT) {
        if (!xr_xir_integer_payload_valid(op->type, op->immediate)) return XR_XIR_BAD_TYPE;
    } else if (op->op == XR_XIR_CONST_FLOAT) {
        if (!xr_xir_float_payload_valid(op->type, op->immediate)) return XR_XIR_BAD_TYPE;
    } else if (op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM) {
        if (op->immediate != 1 && op->immediate != 2) return XR_XIR_BAD_STRUCTURE;
    } else if (op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR) {
        if (op->immediate < 0 || (uint64_t)op->immediate > function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    } else if (op->op != XR_XIR_CONST_INT && op->op != XR_XIR_CLEANUP_REGISTER && op->op != XR_XIR_CALL &&
               op->op != XR_XIR_FUNCTION_REF && op->op != XR_XIR_INVOKE && op->op != XR_XIR_INVOKE_INDIRECT &&
               op->op != XR_XIR_INVOKE_RESULT && op->op != XR_XIR_INVOKE_ERROR && op->op != XR_XIR_CALL_INDIRECT &&
               op->op != XR_XIR_CONST_STRING && op->op != XR_XIR_SLOT_LOAD &&
               op->op != XR_XIR_SLOT_INIT && op->op != XR_XIR_SLOT_STORE &&
               op->op != XR_XIR_SLOT_PLACE && op->op != XR_XIR_FIELD_PLACE && op->op != XR_XIR_STRUCT_GET && op->op != XR_XIR_STRUCT_SET &&
               op->op != XR_XIR_ENUM_NEW && op->op != XR_XIR_ENUM_GET && op->op != XR_XIR_ERROR_IS && op->immediate) {
        return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus type_use_context(VerifyContext *context, uint32_t function, XrXirType type) {
    const XrXirGeneric *generic = context->module->generics ? &context->module->generics[function] : NULL;
    XrXirStatus status = xr_xir_type_context_verify(context->module->types, type,
        generic ? generic->constraints : NULL, generic ? generic->parameter_count : 0, &context->remaining);
    /* Naming checks for substituted types are discharged by original-definition
     * verification and complete correspondence before this module can publish. */
    if (status != XR_XIR_OK || context->module->provenance) return status;
    return xr_xir_type_access(context->module, function, type, &context->remaining);
}

static XrXirStatus function_shape(const XrXirFunction *function, XrXirStage stage,
                                 VerifyContext *context) {
    if (!spend_count(&context->remaining.parameters, function->parameter_count) ||
        !spend_count(&context->remaining.blocks, function->block_count) ||
        !spend_count(&context->remaining.instructions, function->instruction_count))
        return XR_XIR_BUDGET;
    uint64_t bytes = sizeof(*function) + (uint64_t) function->name_length +
        (uint64_t) function->parameter_count * sizeof(*function->parameters) +
        (uint64_t) function->block_count * sizeof(*function->blocks) +
        (uint64_t) function->operand_count * sizeof(*function->operands) +
        (uint64_t) function->instruction_count * sizeof(*function->instructions);
    if (!spend(&context->remaining.metadata_bytes, bytes) || bytes > SIZE_MAX)
        return XR_XIR_BUDGET;
    if (!function->name || !function->name_length || !function->blocks ||
        !function->block_count || !function->instructions || !function->instruction_count ||
        (function->parameter_count && !function->parameters) ||
        (function->operand_count && !function->operands) || (!function->operand_count && function->operands) ||
        function->parameter_count > 65536 ||
        function->parameter_count > UINT32_MAX - function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t function_id = (uint32_t) (function - context->module->functions);
    if (xr_xir_type_is_cell(context->module->types, function->result)) return XR_XIR_BAD_TYPE;
    if (function->result != XR_XIR_UNIT && !xr_xir_type_in_context(context->module, function_id, function->result))
        return XR_XIR_BAD_TYPE;
    XrXirStatus visibility = type_use_context(context, function_id, function->result);
    if (visibility != XR_XIR_OK) return visibility;
    if (!spend(&context->remaining.work, function->name_length))
        return XR_XIR_BUDGET;
    if (memchr(function->name, 0, function->name_length))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        if (!xr_xir_type_in_context(context->module, function_id, function->parameters[p]))
            return XR_XIR_BAD_TYPE;
        visibility = type_use_context(context, function_id, function->parameters[p]);
        if (visibility != XR_XIR_OK) return visibility;
        if (xr_xir_type_is_cell(context->module->types, function->parameters[p]) && (!context->module->declarations ||
            context->module->declarations->functions[function_id].exported)) return XR_XIR_BAD_TYPE;
    }
    uint32_t end = 0, operand_end = 0, type_end = 0;
    for (uint32_t b = 0; b < function->block_count; ++b) {
        context->location.block = b;
        const XrXirBlock *block = &function->blocks[b];
        if (!block->count || block->first != end ||
            block->count > function->instruction_count - end ||
            block->panic >= function->block_count || (block->panic && (block->panic == b || !b)))
            return XR_XIR_BAD_STRUCTURE;
        end += block->count;
        for (uint32_t i = block->first; i < end; ++i) {
            context->location.instruction = i;
            if (!spend(&context->remaining.work, 1))
                return XR_XIR_BUDGET;
            const XrXirInstruction *op = &function->instructions[i];
            visibility = type_use_context(context, function_id, op->type);
            if (visibility != XR_XIR_OK) return visibility;
            if ((xr_xir_op_references_function(op->op)) && !spend(&context->remaining.work, op->type_arguments[1])) return XR_XIR_BUDGET;
            if ((xr_xir_op_references_function(op->op)) && context->module->declarations) {
                const XrXirDeclarations *d = context->module->declarations;
                uint32_t caller = (uint32_t) (function - context->module->functions);
                if (!spend(&context->remaining.work, d->modules[d->functions[caller].module].dependency_count))
                    return XR_XIR_BUDGET;
            }
            if (op->op == XR_XIR_FUNCTION_REF && op->immediate >= 0 &&
                (uint64_t) op->immediate < context->module->function_count &&
                !spend(&context->remaining.work, context->module->functions[op->immediate].parameter_count))
                return XR_XIR_BUDGET;
            XrXirStatus status = instruction_shape(function, op, stage, context->module, &context->remaining);
            if (status != XR_XIR_OK)
                return status;
            if ((xr_xir_op_references_function(op->op)) && op->type_arguments[1]) {
                if (op->type_arguments[0] != type_end) return XR_XIR_BAD_STRUCTURE;
                type_end += op->type_arguments[1];
            }
            if (xr_xir_op_uses_operand_table(op->op) && op->args[1]) {
                if (op->args[0] != operand_end) return XR_XIR_BAD_STRUCTURE;
                operand_end += op->args[1];
            }
            if (op->op == XR_XIR_PHI && (!b || (i != block->first && function->instructions[i - 1].op != XR_XIR_PHI)))
                return XR_XIR_BAD_STRUCTURE;
            if (op_rules[op->op].terminal != (i == end - 1))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    uint32_t type_count = context->module->generics ? context->module->generics[function_id].argument_count : 0;
    return end == function->instruction_count && operand_end == function->operand_count && type_end == type_count ?
        XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}

static void graph_free(Graph *graph) {
    xr_free(graph->owner);
    xr_free(graph->queue);
    xr_free(graph->head);
    xr_free(graph->fault_head);
    xr_free(graph->predecessor);
    xr_free(graph->next);
    xr_free(graph->reachable);
    xr_free(graph->dominators);
    xr_free(graph->meet);
}

static XrXirStatus graph_allocate(Graph *graph, const XrXirFunction *function,
                                VerifyContext *context) {
    uint64_t blocks = function->block_count;
    graph->words = (uint32_t) ((blocks + 63) / 64);
    uint64_t bytes = (uint64_t) function->instruction_count * sizeof(uint32_t) +
        blocks * (9 * sizeof(uint32_t) + sizeof(uint8_t)) +
        (blocks + 1) * graph->words * sizeof(uint64_t);
    if (bytes > context->remaining.scratch_bytes || bytes > SIZE_MAX || blocks > UINT32_MAX / 3)
        return XR_XIR_BUDGET;
    graph->owner = xr_calloc(function->instruction_count, sizeof(uint32_t));
    graph->queue = xr_calloc((size_t) blocks, sizeof(uint32_t));
    graph->head = xr_calloc((size_t) blocks, sizeof(uint32_t));
    graph->fault_head = xr_calloc((size_t) blocks, sizeof(uint32_t));
    graph->predecessor = xr_calloc((size_t) blocks * 3, sizeof(uint32_t));
    graph->next = xr_calloc((size_t) blocks * 3, sizeof(uint32_t));
    graph->reachable = xr_calloc((size_t) blocks, sizeof(uint8_t));
    graph->dominators = xr_calloc((size_t) blocks * graph->words, sizeof(uint64_t));
    graph->meet = xr_calloc(graph->words, sizeof(uint64_t));
    if (!graph->owner || !graph->queue || !graph->head || !graph->fault_head || !graph->predecessor ||
        !graph->next || !graph->reachable || !graph->dominators || !graph->meet)
        return XR_XIR_OUT_OF_MEMORY;
    return XR_XIR_OK;
}

static const XrXirInstruction *terminator(const XrXirFunction *function, uint32_t block) {
    const XrXirBlock *range = &function->blocks[block];
    return &function->instructions[range->first + range->count - 1];
}

static XrXirStatus graph_connect(Graph *graph, const XrXirFunction *function,
                               VerifyContext *context) {
    uint32_t edge = 0;
    for (uint32_t b = 0; b < function->block_count; ++b)
        graph->head[b] = graph->fault_head[b] = UINT32_MAX;
    for (uint32_t b = 0; b < function->block_count; ++b) {
        const XrXirBlock *block = &function->blocks[b];
        if (!spend(&context->remaining.work, (uint64_t) block->count + 1))
            return XR_XIR_BUDGET;
        for (uint32_t i = block->first; i < block->first + block->count; ++i)
            graph->owner[i] = b;
        const XrXirInstruction *op = terminator(function, b);
        for (uint32_t i = 0; i < op_rules[op->op].edges; ++i) {
            uint32_t target = op->targets[i];
            graph->predecessor[edge] = b;
            graph->next[edge] = graph->head[target];
            graph->head[target] = edge++;
        }
        if (block->panic) {
            graph->predecessor[edge] = b;
            graph->next[edge] = graph->fault_head[block->panic];
            graph->fault_head[block->panic] = edge++;
        }
    }
    uint32_t front = 0, count = 1;
    graph->reachable[0] = 1;
    while (front < count) {
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        uint32_t block = graph->queue[front++];
        const XrXirInstruction *op = terminator(function, block);
        for (uint32_t i = 0; i <= op_rules[op->op].edges; ++i) {
            uint32_t target = i < op_rules[op->op].edges ? op->targets[i] : function->blocks[block].panic;
            if (target && !graph->reachable[target]) {
                graph->reachable[target] = 1;
                graph->queue[count++] = target;
            }
        }
    }
    if (count != function->block_count)
        return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus graph_dominators(Graph *graph, const XrXirFunction *function,
                                  VerifyContext *context) {
    graph->dominators[0] = 1;
    for (uint32_t b = 1; b < function->block_count; ++b) {
        if (!spend(&context->remaining.work, graph->words))
            return XR_XIR_BUDGET;
        for (uint32_t w = 0; w < graph->words; ++w)
            graph->dominators[(size_t) b * graph->words + w] = UINT64_MAX;
    }
    bool changed;
    do {
        changed = false;
        for (uint32_t b = 1; b < function->block_count; ++b) {
            if (!spend(&context->remaining.work, (uint64_t) graph->words * 2))
                return XR_XIR_BUDGET;
            for (uint32_t w = 0; w < graph->words; ++w)
                graph->meet[w] = UINT64_MAX;
            for (uint32_t edge = graph->head[b]; edge != UINT32_MAX; edge = graph->next[edge]) {
                if (!spend(&context->remaining.work, graph->words))
                    return XR_XIR_BUDGET;
                size_t offset = (size_t) graph->predecessor[edge] * graph->words;
                for (uint32_t w = 0; w < graph->words; ++w)
                    graph->meet[w] &= graph->dominators[offset + w];
            }
            for (uint32_t edge = graph->fault_head[b]; edge != UINT32_MAX; edge = graph->next[edge]) {
                if (!spend(&context->remaining.work, graph->words))
                    return XR_XIR_BUDGET;
                uint32_t from = graph->predecessor[edge];
                size_t offset = (size_t) from * graph->words;
                for (uint32_t w = 0; w < graph->words; ++w)
                    graph->meet[w] &= graph->dominators[offset + w] &
                        ~(w == from / 64 ? UINT64_C(1) << (from % 64) : 0);
            }
            graph->meet[b / 64] |= UINT64_C(1) << (b % 64);
            size_t offset = (size_t) b * graph->words;
            for (uint32_t w = 0; w < graph->words; ++w) {
                if (graph->dominators[offset + w] != graph->meet[w])
                    changed = true;
                graph->dominators[offset + w] = graph->meet[w];
            }
        }
    } while (changed);
    return XR_XIR_OK;
}

static XrXirStatus value_use(const XrXirFunction *function, const Graph *graph,
                            uint32_t instruction, uint32_t value, XrXirType expected) {
    if (value < function->parameter_count)
        return function->parameters[value] == expected ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    uint32_t definition = value - function->parameter_count;
    if (definition >= function->instruction_count ||
        function->instructions[definition].type == XR_XIR_UNIT ||
        op_rules[function->instructions[definition].op].terminal)
        return XR_XIR_BAD_VALUE;
    if (function->instructions[definition].type != expected)
        return XR_XIR_BAD_TYPE;
    uint32_t from = graph->owner[definition], to = graph->owner[instruction];
    if (from == to)
        return definition < instruction ? XR_XIR_OK : XR_XIR_BAD_DOMINANCE;
    uint64_t bits = graph->dominators[(size_t) to * graph->words + from / 64];
    return (bits & (UINT64_C(1) << (from % 64))) ? XR_XIR_OK : XR_XIR_BAD_DOMINANCE;
}

static bool local_access(XrXirOp op) {
    return op == XR_XIR_CELL_LOCAL_WRITE || op == XR_XIR_LOCAL_READ || op == XR_XIR_LOCAL_WRITE ||
        op == XR_XIR_SCALAR_LOCAL_READ || op == XR_XIR_SCALAR_LOCAL_WRITE ||
        op == XR_XIR_OWNED_LOCAL_READ || op == XR_XIR_OWNED_LOCAL_WRITE;
}
static bool local_write(XrXirOp op) {
    return op == XR_XIR_CELL_LOCAL_WRITE || op == XR_XIR_LOCAL_WRITE || op == XR_XIR_SCALAR_LOCAL_WRITE || op == XR_XIR_OWNED_LOCAL_WRITE;
}
static XrXirStatus local_operand(const XrXirTypes *types, const XrXirFunction *function, const XrXirInstruction *op,
                                  uint32_t id, uint32_t operand) {
    XrXirPlaceKind place = xr_xir_place_kind(function, id);
    bool local = operand == 0 && local_access(op->op);
    if (local ? place != XR_XIR_PLACE_LOCAL : place != XR_XIR_PLACE_NONE) return XR_XIR_BAD_VALUE;
    if (local) {
        XrXirType type = function->instructions[id - function->parameter_count].type;
        if ((op->op == XR_XIR_CELL_LOCAL_WRITE && !xr_xir_type_is_cell(types, type)) ||
            (op->op == XR_XIR_SCALAR_LOCAL_WRITE && !scalar(type)) ||
            (op->op == XR_XIR_OWNED_LOCAL_WRITE && !xr_xir_type_is_owned(types, type))) return XR_XIR_BAD_TYPE;
    }
    return XR_XIR_OK;
}

static XrXirStatus phi_uses(const Graph *graph, const XrXirFunction *function,
                           VerifyContext *context, uint32_t index) {
    const XrXirInstruction *op = &function->instructions[index];
    uint32_t block = graph->owner[index], predecessors = 0, previous = UINT32_MAX;
    for (uint32_t edge = graph->head[block]; edge != UINT32_MAX; edge = graph->next[edge]) {
        if (!spend(&context->remaining.work, 1)) return XR_XIR_BUDGET;
        uint32_t from = graph->predecessor[edge];
        if (from != previous) ++predecessors;
        previous = from;
    }
    if (op->args[1] / 2 != predecessors) return XR_XIR_BAD_STRUCTURE;
    previous = UINT32_MAX;
    for (uint32_t a = 0; a < op->args[1]; a += 2) {
        if (!spend(&context->remaining.work, 1)) return XR_XIR_BUDGET;
        uint32_t from = function->operands[op->args[0] + a];
        uint32_t value = function->operands[op->args[0] + a + 1];
        if (from >= function->block_count || (a && from <= previous)) return XR_XIR_BAD_STRUCTURE;
        previous = from;
        const XrXirInstruction *end = terminator(function, from);
        bool connected = false;
        for (uint32_t e = 0; e < op_rules[end->op].edges; ++e)
            if (end->targets[e] == block) connected = true;
        if (!connected) return XR_XIR_BAD_STRUCTURE;
        XrXirStatus status = local_operand(context->module->types, function, op, value, 0);
        if (status == XR_XIR_OK) status = value_use(function, graph,
            function->blocks[from].first + function->blocks[from].count - 1, value, op->type);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus place_write_authority(const XrXirFunction *function,
    VerifyContext *context, uint32_t id) {
    for (;;) {
        if (!spend(&context->remaining.work, 1)) return XR_XIR_BUDGET;
        XrXirPlaceKind place = xr_xir_place_kind(function, id);
        if (place == XR_XIR_PLACE_NONE) return XR_XIR_BAD_VALUE;
        const XrXirInstruction *op = &function->instructions[id - function->parameter_count];
        if (place == XR_XIR_PLACE_FIELD) {
            XrXirType type = xr_xir_operand_type(function, op->args[0]);
            const XrXirTypeNode *node = xr_xir_type_node(context->module->types, type);
            if (!node || !xr_xir_type_is_struct(context->module->types, type) || op->immediate < 0 ||
                (uint64_t)op->immediate > UINT32_MAX) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xr_xir_nominal_access(context->module, context->location.function,
                node->nominal.declaration, (uint32_t)op->immediate, XR_XIR_NOMINAL_WRITE, &context->remaining.work);
            if (status != XR_XIR_OK) return status;
        } else if (place == XR_XIR_PLACE_SLOT) {
            const XrXirDeclarations *d = context->module->declarations;
            if (!d || op->immediate < 0 || (uint64_t)op->immediate >= d->slot_count) return XR_XIR_BAD_STRUCTURE;
            const XrXirSlot *slot = &d->slots[op->immediate];
            return slot->mutable && slot->module == d->root_module ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
        } else if (place != XR_XIR_PLACE_INDEX) return XR_XIR_OK;
        id = op->args[0];
    }
}
static XrXirStatus role_operand(const XrXirFunction *function, const Graph *graph,
                                VerifyContext *context, uint32_t instruction,
                                uint32_t ordinal, XrXirType expected) {
    const XrXirInstruction *op = &function->instructions[instruction];
    uint32_t id = xr_xir_op_uses_operand_table(op->op) ?
        function->operands[op->args[0] + ordinal] : op->args[ordinal];
    XrXirPlaceKind place = xr_xir_place_kind(function, id);
    XrXirOperandRole role = xr_xir_operand_role(op->op, ordinal);
    if ((role == XR_XIR_OPERAND_VALUE && place != XR_XIR_PLACE_NONE) ||
        (role == XR_XIR_OPERAND_WRITE && place == XR_XIR_PLACE_NONE)) return XR_XIR_BAD_VALUE;
    if (role == XR_XIR_OPERAND_WRITE) {
        XrXirStatus status = place_write_authority(function, context, id);
        if (status != XR_XIR_OK) return status;
    }
    return value_use(function, graph, instruction, id, expected);
}

static XrXirStatus array_uses(const Graph *graph, const XrXirFunction *function,
                             VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op = &function->instructions[instruction];
    if (op->op == XR_XIR_SLOT_PLACE) return XR_XIR_OK;
    const XrXirTypes *types = context->module->types;
    XrXirType array = op->type, cell = XR_XIR_UNIT;
    if (op->op == XR_XIR_CELL_PLACE) {
        cell = xr_xir_operand_type(function, op->args[0]);
        if (!xr_xir_type_is_cell(types, cell) || xr_xir_cell_element(types, cell) != array)
            return XR_XIR_BAD_TYPE;
    } else if (op->op != XR_XIR_ARRAY_NEW) {
        uint32_t receiver = op->op == XR_XIR_ARRAY_SET ? function->operands[op->args[0]] : op->args[0];
        array = xr_xir_operand_type(function, receiver);
        if (!xr_xir_type_is_array(types, array)) return XR_XIR_BAD_TYPE;
    }
    if (op->op == XR_XIR_CELL_PLACE && xr_xir_type_is_nominal(types, array))
        return role_operand(function, graph, context, instruction, 0, cell);
    XrXirType element = xr_xir_array_element(types, array);
    XrXirStatus status = context->module->provenance ?
        xr_xir_type_constraints(context->module, context->location.function, element, 0, &context->remaining) :
        xr_xir_type_satisfies(context->module, context->location.function, element, 0, &context->remaining);
    if (status != XR_XIR_OK) return status;
    if (op->op == XR_XIR_ARRAY_GET && op->type != element) return XR_XIR_BAD_TYPE;
    uint32_t count = operand_count(function, op, context->module);
    if (!spend(&context->remaining.work, count)) return XR_XIR_BUDGET;
    for (uint32_t a = 0; a < count; ++a) {
        XrXirType expected = a ? element : array;
        if (op->op == XR_XIR_CELL_PLACE) expected = cell;
        if (op->op == XR_XIR_ARRAY_NEW) expected = element;
        if ((op->op == XR_XIR_ARRAY_GET || op->op == XR_XIR_ARRAY_SET) && a == 1) expected = XR_XIR_I64;
        status = role_operand(function, graph, context, instruction, a, expected);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus struct_uses(const Graph *graph, const XrXirFunction *function,
                              VerifyContext *context, uint32_t instruction) {
    const XrXirInstruction *op = &function->instructions[instruction];
    bool construct = op->op == XR_XIR_STRUCT_NEW, write = op->op == XR_XIR_STRUCT_SET;
    XrXirType type = construct ? op->type : xr_xir_operand_type(function, op->args[0]);
    const XrXirTypes *types = context->module->types;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    if (!node || !xr_xir_type_is_struct(types, type)) return XR_XIR_BAD_TYPE;
    const XrXirNominalType *instance = &node->nominal;
    const XrXirNominalTable *table = types->nominals;
    const XrXirNominalDeclaration *declaration = table->declarations ?
        &table->declarations[instance->declaration] : NULL;
    uint32_t fields = declaration ? declaration->field_count : table->identities[instance->declaration].field_count;
    if (construct ? op->args[1] != fields : op->immediate < 0 || (uint64_t) op->immediate >= fields)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xr_xir_nominal_access(context->module, context->location.function,
        instance->declaration, construct ? 0 : (uint32_t) op->immediate,
        construct ? XR_XIR_NOMINAL_CONSTRUCT : write ? XR_XIR_NOMINAL_WRITE : XR_XIR_NOMINAL_READ, &context->remaining.work);
    if (status != XR_XIR_OK) return status;
    if (write) {
        status = role_operand(function, graph, context, instruction, 0, type);
        if (status != XR_XIR_OK) return status;
    }
    uint32_t count = construct ? fields : 1;
    if (!spend(&context->remaining.work, count)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t field = construct ? i : (uint32_t) op->immediate;
        uint32_t value = construct ? function->operands[op->args[0] + i] : op->args[write ? 1 : 0];
        XrXirType actual = construct || write ? xr_xir_operand_type(function, value) : op->type;
        if (declaration) status = xr_xir_type_substitution_matches(types, instance->arguments,
            instance->argument_count, declaration->fields[field].type, actual, &context->remaining);
        else status = instance->field_count == fields && instance->fields[field] == actual ? XR_XIR_OK : XR_XIR_BAD_TYPE;
        if (status == XR_XIR_OK) status = local_operand(types, function, op, value, write ? 1 : i);
        if (status == XR_XIR_OK) status = value_use(function, graph, instruction, value, construct || write ? actual : type);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

#include "xxir_path_verify.inc.c"
#include "xxir_enum_verify.inc.c"

#include "xxir_invoke_verify.inc.c"
#include "xxir_error_verify.inc.c"
#include "xxir_panic_verify.inc.c"

static XrXirStatus graph_uses(const Graph *graph, const XrXirFunction *function,
                            VerifyContext *context) {
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        context->location.block = graph->owner[i];
        context->location.instruction = i;
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op >= XR_XIR_FIELD_PLACE && op->op <= XR_XIR_PLACE_WRITE) {
            XrXirStatus status = path_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        if (op->op == XR_XIR_ERROR_IS || op->op == XR_XIR_ERROR_NARROW) {
            XrXirStatus status = error_filter_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        if (op->op >= XR_XIR_ENUM_NEW && op->op <= XR_XIR_ENUM_GET) {
            XrXirStatus status = enum_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        if (op->op >= XR_XIR_STRUCT_NEW && op->op <= XR_XIR_STRUCT_SET) {
            XrXirStatus status = struct_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        if (op->op >= XR_XIR_CELL_PLACE && op->op <= XR_XIR_ARRAY_LEN) {
            XrXirStatus status = array_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        if (op->op == XR_XIR_PHI) {
            XrXirStatus status = phi_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        XrXirType expected = op->type;
        if (op->op == XR_XIR_FUNCTION_WEAKEN) {
            expected = xr_xir_operand_type(function, op->args[0]);
            XrXirStatus status = xr_xir_callable_weakening(context->module->types,
                expected, op->type, &context->remaining.work);
            if (status != XR_XIR_OK) return status;
        }
        if (op->op == XR_XIR_RETURN)
            expected = function->result;
        else if (op->op == XR_XIR_BRANCH)
            expected = XR_XIR_BOOL;
        else if (op->op == XR_XIR_EQ_INT || op->op == XR_XIR_LT_INT ||
                 (op->op >= XR_XIR_NE_INT && op->op <= XR_XIR_GE_INT)) {
            expected = xr_xir_operand_type(function, op->args[0]);
            if (!xr_xir_type_is_integer(expected)) return XR_XIR_BAD_TYPE;
        }
        if (op->op == XR_XIR_CONVERT_NUMBER || (op->op >= XR_XIR_EQ_FLOAT && op->op <= XR_XIR_GE_FLOAT)) {
            expected = xr_xir_operand_type(function, op->args[0]);
            if (op->op == XR_XIR_CONVERT_NUMBER ? !xr_xir_type_is_number(expected) : !xr_xir_float_bits(expected))
                return XR_XIR_BAD_TYPE;
        }
        if (op->op == XR_XIR_THROW || op->op == XR_XIR_CLEANUP_ERROR || op->op == XR_XIR_ERROR_ERASE) {
            if (op->op == XR_XIR_ERROR_ERASE && op->type != XR_XIR_ERROR) return XR_XIR_BAD_TYPE;
            expected = xr_xir_operand_type(function, op->args[0]);
            XrXirStatus status = xr_xir_type_constraints(context->module, context->location.function,
                expected, XR_XIR_CONSTRAINT_ERROR, &context->remaining);
            if (status != XR_XIR_OK) return status;
        }
        if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE)
            expected = context->module->declarations->slots[op->immediate].type;
        if (op->op == XR_XIR_ATOMIC_I64_NEW) expected = XR_XIR_I64;
        if (op->op == XR_XIR_CELL_NEW) expected = xr_xir_cell_element(context->module->types, op->type);
        if (op->op == XR_XIR_CELL_READ) {
            expected = xr_xir_operand_type(function, op->args[0]);
            if (!xr_xir_type_is_cell(context->module->types, expected) ||
                xr_xir_cell_element(context->module->types, expected) != op->type) return XR_XIR_BAD_TYPE;
        }
        if (op->op == XR_XIR_CELL_WRITE) {
            expected = xr_xir_operand_type(function, op->args[0]);
            if (!xr_xir_type_is_cell(context->module->types, expected)) return XR_XIR_BAD_TYPE;
        }
        if (op->op == XR_XIR_WRITE_STREAM || op->op == XR_XIR_STRING_LEN ||
            op->op == XR_XIR_EQ_STRING || op->op == XR_XIR_NE_STRING ||
            op->op == XR_XIR_STRING_CONTAINS || op->op == XR_XIR_STRING_STARTS_WITH ||
            op->op == XR_XIR_STRING_ENDS_WITH || op->op == XR_XIR_STRING_INDEX_OF ||
            op->op == XR_XIR_STRING_LAST_INDEX_OF) expected = XR_XIR_STRING;
        if (op->op == XR_XIR_ATOMIC_I64_LOAD || op->op == XR_XIR_ATOMIC_I64_FETCH_ADD)
            expected = XR_XIR_ATOMIC_I64;
        if (op->op == XR_XIR_PANIC_CODE || op->op == XR_XIR_PANIC_MESSAGE) expected = XR_XIR_PANIC_INFO;
        if (local_write(op->op)) {
            if (op->args[0] < function->parameter_count ||
                op->args[0] - function->parameter_count >= function->instruction_count) return XR_XIR_BAD_VALUE;
            expected = function->instructions[op->args[0] - function->parameter_count].type;
        }
        const XrXirTypeNode *indirect = NULL;
        if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
            uint32_t callee = (uint32_t) op->immediate;
            XrXirType type = xr_xir_operand_type(function, callee);
            indirect = xr_xir_callable_signature(context->module->types, type);
            XrXirStatus status = local_operand(context->module->types, function, op, callee, 0);
            if (status == XR_XIR_OK) status = value_use(function, graph, i, callee, type);
            if (status != XR_XIR_OK) return status;
        }
        uint32_t count = operand_count(function, op, context->module);
        if (!spend(&context->remaining.work, count)) return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < count; ++a) {
            XrXirType operand_type = indirect ? indirect->parameters[a].type : expected;
            if (xr_xir_op_references_function(op->op)) {
                uint32_t operand = function->operands[op->args[0] + a];
                if (operand >= (uint64_t) function->parameter_count + function->instruction_count) return XR_XIR_BAD_VALUE;
                operand_type = xr_xir_operand_type(function, operand);
                XrXirStatus match = xr_xir_call_type_matches(context->module, context->location.function, op,
                    context->module->functions[op->immediate].parameters[a], operand_type, &context->remaining);
                if (match != XR_XIR_OK) return match;
            }
            if ((op->op == XR_XIR_SHL_INT || op->op == XR_XIR_SHR_INT) && a == 1) {
                operand_type = xr_xir_operand_type(function, op->args[1]);
                if (!xr_xir_type_is_integer(operand_type)) return XR_XIR_BAD_TYPE;
            }
            if (op->op == XR_XIR_STRING_INDEX_OF && a == 2) operand_type = XR_XIR_I64;
            if (op->op == XR_XIR_ATOMIC_I64_FETCH_ADD && a == 1) operand_type = XR_XIR_I64;
            if ((op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_CELL_LOCAL_WRITE) && a == 1) operand_type = xr_xir_cell_element(context->module->types, expected);
            if (op->op == XR_XIR_OUTPUT || op->op == XR_XIR_PRINT) {
                uint32_t id = op->op == XR_XIR_PRINT ? function->operands[op->args[0] + a] : op->args[a];
                if (id >= function->parameter_count + function->instruction_count) return XR_XIR_BAD_VALUE;
                operand_type = id < function->parameter_count ? function->parameters[id] :
                    function->instructions[id - function->parameter_count].type;
                if (operand_type == XR_XIR_UNIT) return XR_XIR_BAD_VALUE;
                if (operand_type != XR_XIR_BOOL && !xr_xir_type_is_number(operand_type) && operand_type != XR_XIR_STRING) return XR_XIR_BAD_TYPE;
            }
            uint32_t id = xr_xir_op_references_function(op->op) || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_PRINT || op->op == XR_XIR_STRING_INDEX_OF ?
                function->operands[op->args[0] + a] : op->args[a];
            XrXirStatus status = local_operand(context->module->types, function, op, id, a);
            if (status == XR_XIR_OK) status = value_use(function, graph, i, id, operand_type);
            if (status != XR_XIR_OK)
                return status;
        }
    }
    return XR_XIR_OK;
}

#include "xxir_initialization.inc.c"
#include "xxir_cleanup_frontier.inc.c"
static XrXirStatus verify_function(const XrXirFunction *function, XrXirStage stage,
                                 VerifyContext *context) {
    XrXirStatus status = function_shape(function, stage, context);
    if (status != XR_XIR_OK)
        return status;
    Graph graph = {0};
    status = graph_allocate(&graph, function, context);
    if (status == XR_XIR_OK)
        status = graph_connect(&graph, function, context);
    if (status == XR_XIR_OK)
        status = graph_dominators(&graph, function, context);
    if (status == XR_XIR_OK)
        status = invoke_edges(&graph, function, context);
    if (status == XR_XIR_OK)
        status = panic_edges(&graph, function, context);
    if (status == XR_XIR_OK)
        status = cleanup_frontiers(&graph, function, context);
    if (status == XR_XIR_OK)
        status = graph_uses(&graph, function, context);
    if (status == XR_XIR_OK)
        status = initialization_check(function, NULL, context);
    graph_free(&graph);
    return status;
}

static XrXirStatus verify_nominal_modules(const XrXirModule *module, XrXirBudget *remaining) {
    const XrXirNominalTable *table = module->types ? module->types->nominals : NULL;
    if (!table) return XR_XIR_OK;
    if (!module->declarations) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirLiteral name = table->declarations ? table->declarations[i].module : table->identities[i].module;
        bool found = false;
        for (uint32_t m = 0; m < module->declarations->module_count; ++m) {
            const XrXirSourceModule *owner = &module->declarations->modules[m];
            uint64_t cost = owner->name_length == name.length ? (uint64_t) name.length + 1 : 1;
            if (!spend(&remaining->work, cost)) return XR_XIR_BUDGET;
            if (owner->name_length == name.length && !memcmp(owner->name, name.bytes, name.length)) {
                if (table->declarations) {
                    const XrXirNominalDeclaration *d = &table->declarations[i];
                    for (uint32_t f = 0; f < d->field_count; ++f) {
                        XrXirStatus status = xr_xir_type_access(module, owner->initializer, d->fields[f].type, remaining);
                        if (status != XR_XIR_OK) return status;
                    }
                }
                found = true; break;
            }
        }
        if (!found) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

static XrXirStatus verify_provenance(const XrXirModule *module, XrXirBudget *remaining,
    XrXirDiagnostic *diagnostic) {
    const XrXirProvenance *p = module->provenance;
    if (module->stage != XR_XIR_CHECKED && module->stage != XR_XIR_LOWERED) return XR_XIR_BAD_STAGE;
    if (!p->source || p->source->module.provenance || !p->origins || p->count != module->function_count)
        return XR_XIR_BAD_STRUCTURE;
    if (p->source->module.stage != XR_XIR_CHECKED) return XR_XIR_BAD_STAGE;
    uint64_t bytes = sizeof(*p) + (uint64_t)p->count * sizeof(*p->origins);
    if (!spend(&remaining->metadata_bytes, bytes) || !spend(&remaining->work, p->count)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < p->count; ++i) {
        bytes = (uint64_t)p->origins[i].argument_count * sizeof(XrXirType);
        if (!spend(&remaining->metadata_bytes, bytes)) return XR_XIR_BUDGET;
    }
    XrXirStatus status = xr_xir_verify_remaining(&p->source->module, remaining, diagnostic);
    if (status != XR_XIR_OK) return status;
    return xr_xir_provenance_functions_match(&p->source->module, module, p->origins, remaining, diagnostic);
}

#include "xxir_effect_obligations.inc.c"

XrXirStatus xr_xir_verify_remaining(const XrXirModule *module, XrXirBudget *remaining,
                                  XrXirDiagnostic *diagnostic) {
    if (!remaining) {
        if (diagnostic)
            *diagnostic = (XrXirDiagnostic) {XR_XIR_BAD_STRUCTURE, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
        return XR_XIR_BAD_STRUCTURE;
    }
    VerifyContext context = {*remaining,
                             {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE}, module};
    XrXirStatus status = XR_XIR_OK;
    if (!module || !module->functions || !module->function_count)
        status = XR_XIR_BAD_STRUCTURE;
    else if (module->stage != XR_XIR_BUILT && module->stage != XR_XIR_CHECKED &&
             module->stage != XR_XIR_LOWERED)
        status = XR_XIR_BAD_STAGE;
    else if (!spend_count(&context.remaining.functions, module->function_count) ||
             !spend(&context.remaining.metadata_bytes, sizeof(XrXirArtifact)))
        status = XR_XIR_BUDGET;
    if (status == XR_XIR_OK) {
        status = xr_xir_types_verify(module->types, &context.remaining);
        if (status == XR_XIR_OK && module->types && module->types->nominals) {
            bool identities = module->types->nominals->identities != NULL;
            if (identities != (module->stage == XR_XIR_LOWERED)) status = XR_XIR_BAD_STAGE;
        }
    }
    if (status == XR_XIR_OK) {
        if (module->types && ((!module->generics && !module->types->nominals) || module->stage == XR_XIR_LOWERED))
            for (uint32_t t = 0; t < module->types->count; ++t)
                if (module->types->nodes[t].parameter_span) status = XR_XIR_BAD_TYPE;
    }
    if (status == XR_XIR_OK) {
        status = xr_xir_declarations_verify(module->declarations, module->types, module->function_count,
            &context.remaining.metadata_bytes, &context.remaining.work);
    }
    if (status == XR_XIR_OK) status = verify_nominal_modules(module, &context.remaining);
    if (status == XR_XIR_OK) status = xr_xir_generics_verify(module, &context.remaining);
    if (status == XR_XIR_OK && module->declarations) {
        const XrXirDeclarations *d = module->declarations;
        const XrXirFunction *entry = &module->functions[d->entry_function];
        if (entry->parameter_count || entry->result != XR_XIR_I64 ||
            (module->generics && module->generics[d->entry_function].parameter_count)) status = XR_XIR_BAD_TYPE;
        for (uint32_t m = 0; m < d->module_count && status == XR_XIR_OK; ++m) {
            const XrXirFunction *init = &module->functions[d->modules[m].initializer];
            if (init->parameter_count || init->result != XR_XIR_UNIT ||
                (module->generics && module->generics[d->modules[m].initializer].parameter_count)) status = XR_XIR_BAD_TYPE;
        }
    }
    if (status == XR_XIR_OK) {
        for (uint32_t f = 0; f < module->function_count; ++f) {
            context.location = (XrXirDiagnostic) {XR_XIR_OK, f, UINT32_MAX, UINT32_MAX, XR_XIR_DIAGNOSTIC_NONE};
            if (!spend(&context.remaining.work, 1)) {
                status = XR_XIR_BUDGET;
                break;
            }
            status = verify_function(&module->functions[f], module->stage, &context);
            if (status != XR_XIR_OK)
                break;
        }
    }
    if (status == XR_XIR_OK && module->provenance)
        status = verify_provenance(module, &context.remaining, &context.location);
    if (status == XR_XIR_OK) status = declaration_effects_verify(module, &context.remaining, &context.location);
    context.location.status = status;
    *remaining = context.remaining;
    if (diagnostic)
        *diagnostic = context.location;
    return status;
}

XrXirStatus xr_xir_verify(const XrXirModule *module, const XrXirBudget *budget,
                        XrXirDiagnostic *diagnostic) {
    XrXirBudget remaining = budget ? *budget : xr_xir_default_budget();
    return xr_xir_verify_remaining(module, &remaining, diagnostic);
}
