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
 */

#include "xxir_internal.h"
#include "xxir_generic.h"
#include "xxir_callable.h"
#include "../base/xmalloc.h"
#include <limits.h>

typedef enum ResultRule {
    RULE_UNIT, RULE_BOOL, RULE_I64, RULE_SCALAR, RULE_VALUE, RULE_OWNED, RULE_STRING, RULE_CALL, RULE_ATOMIC
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
    return type == XR_XIR_BOOL || type == XR_XIR_I64;
}

static uint32_t operand_count(const XrXirFunction *function, const XrXirInstruction *op,
                              const XrXirModule *module) {
    (void) module;
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_PRINT) return op->args[1];
    return op->op == XR_XIR_RETURN ? (function->result != XR_XIR_UNIT) : op_rules[op->op].operands;
}

static XrXirStatus declaration_instruction(const XrXirFunction *function,
                                           const XrXirInstruction *op, const XrXirModule *module) {
    const XrXirDeclarations *d = module->declarations;
    if (op->op == XR_XIR_CONST_STRING)
        return d && op->immediate >= 0 && (uint64_t) op->immediate < d->literal_count ?
            XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_ATOMIC_I64_NEW) return d ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
    if (op->op != XR_XIR_SLOT_LOAD && op->op != XR_XIR_SLOT_INIT && op->op != XR_XIR_SLOT_STORE)
        return XR_XIR_OK;
    if (!d || op->immediate < 0 || (uint64_t) op->immediate >= d->slot_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirSlot *slot = &d->slots[op->immediate];
    uint32_t caller = (uint32_t) (function - module->functions);
    if (d->functions[caller].module != slot->module) return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_SLOT_LOAD && op->type != slot->type) return XR_XIR_BAD_TYPE;
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
    bool range = op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_PRINT || op->op == XR_XIR_PHI;
    if (range && (op->args[1] > 65536 || op->args[0] > function->operand_count ||
        op->args[1] > function->operand_count - op->args[0] || (!op->args[1] && op->args[0])))
        return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_PHI && (!op->args[1] || op->args[1] % 2)) return XR_XIR_BAD_STRUCTURE;
    uint32_t caller_id = (uint32_t) (function - module->functions);
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_FUNCTION_REF) {
        if (op->immediate < 0 || (uint64_t) op->immediate >= module->function_count)
            return XR_XIR_BAD_STRUCTURE;
        const XrXirFunction *callee = &module->functions[op->immediate];
        if ((op->op == XR_XIR_CALL && callee->parameter_count != op->args[1]) || (callee->parameter_count && !callee->parameters))
            return XR_XIR_BAD_STRUCTURE;
        XrXirStatus generic_status = xr_xir_generic_call(module, caller_id, op);
        if (generic_status != XR_XIR_OK) return generic_status;
        XrXirType result = op->type;
        const XrXirCallableSignature *signature = NULL;
        if (op->op == XR_XIR_FUNCTION_REF) {
            signature = xr_xir_callable_signature(module->callables, op->type);
            if (!module->declarations || !signature || signature->parameter_count != callee->parameter_count)
                return XR_XIR_BAD_TYPE;
            result = signature->result;
        }
        generic_status = xr_xir_call_type_matches(module, caller_id, op, callee->result, result, remaining);
        if (generic_status != XR_XIR_OK) return generic_status;
        if (signature) for (uint32_t p = 0; p < callee->parameter_count; ++p) {
            generic_status = xr_xir_call_type_matches(module, caller_id, op, callee->parameters[p], signature->parameters[p].type, remaining);
            if (generic_status != XR_XIR_OK) return generic_status;
        }
        if (module->declarations) {
            const XrXirDeclarations *d = module->declarations;
            uint32_t caller_module = d->functions[caller_id].module;
            uint32_t callee_module = d->functions[op->immediate].module;
            if ((uint32_t) op->immediate == d->modules[callee_module].initializer ||
                !xr_xir_module_imports(d, caller_module, callee_module) ||
                (caller_module != callee_module && !d->functions[op->immediate].exported))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    if (op->op == XR_XIR_CALL_INDIRECT) {
        if (op->immediate < 0 || (uint64_t) op->immediate >= (uint64_t) function->parameter_count + function->instruction_count)
            return XR_XIR_BAD_VALUE;
        XrXirType type = xr_xir_operand_type(function, (uint32_t) op->immediate);
        const XrXirCallableSignature *signature = xr_xir_callable_signature(module->callables, type);
        if (!signature || signature->parameter_count != op->args[1] || signature->result != op->type)
            return XR_XIR_BAD_TYPE;
    }
    if ((rule->result == RULE_UNIT && op->type != XR_XIR_UNIT) ||
        (rule->result == RULE_BOOL && op->type != XR_XIR_BOOL) ||
        (rule->result == RULE_I64 && op->type != XR_XIR_I64) ||
        (rule->result == RULE_OWNED && (!xr_xir_type_is_owned(op->type) || !xr_xir_type_in_context(module, caller_id, op->type))) ||
        (rule->result == RULE_STRING && op->type != XR_XIR_STRING) ||
        (rule->result == RULE_ATOMIC && op->type != XR_XIR_ATOMIC_I64) ||
        (rule->result == RULE_VALUE && !xr_xir_type_in_context(module, caller_id, op->type)) ||
        (rule->result == RULE_SCALAR && !scalar(op->type)))
        return XR_XIR_BAD_TYPE;
    uint32_t operands = operand_count(function, op, module);
    for (uint32_t i = range ? 2 : operands; i < 2; ++i)
        if (op->args[i])
            return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < rule->edges; ++i)
        if (!op->targets[i] || op->targets[i] >= function->block_count)
            return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = op->op == XR_XIR_CALL || op->op == XR_XIR_FUNCTION_REF ? 2 : rule->edges; i < 2; ++i)
        if (op->targets[i])
            return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_CONST_BOOL) {
        if (op->immediate != 0 && op->immediate != 1)
            return XR_XIR_BAD_TYPE;
    } else if (op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM) {
        if (op->immediate != 1 && op->immediate != 2) return XR_XIR_BAD_STRUCTURE;
    } else if (op->op != XR_XIR_CONST_I64 && op->op != XR_XIR_CALL &&
               op->op != XR_XIR_FUNCTION_REF && op->op != XR_XIR_CALL_INDIRECT &&
               op->op != XR_XIR_CONST_STRING && op->op != XR_XIR_SLOT_LOAD &&
               op->op != XR_XIR_SLOT_INIT && op->op != XR_XIR_SLOT_STORE && op->immediate) {
        return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
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
    if (function->result != XR_XIR_UNIT && !xr_xir_type_in_context(context->module, function_id, function->result))
        return XR_XIR_BAD_TYPE;
    if (!spend(&context->remaining.work, function->name_length))
        return XR_XIR_BUDGET;
    if (memchr(function->name, 0, function->name_length))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        if (!xr_xir_type_in_context(context->module, function_id, function->parameters[p]))
            return XR_XIR_BAD_TYPE;
    }
    uint32_t end = 0, operand_end = 0, type_end = 0;
    for (uint32_t b = 0; b < function->block_count; ++b) {
        context->location.block = b;
        const XrXirBlock *block = &function->blocks[b];
        if (!block->count || block->first != end ||
            block->count > function->instruction_count - end)
            return XR_XIR_BAD_STRUCTURE;
        end += block->count;
        for (uint32_t i = block->first; i < end; ++i) {
            context->location.instruction = i;
            if (!spend(&context->remaining.work, 1))
                return XR_XIR_BUDGET;
            const XrXirInstruction *op = &function->instructions[i];
            if ((op->op == XR_XIR_CALL || op->op == XR_XIR_FUNCTION_REF) && !spend(&context->remaining.work, op->targets[1])) return XR_XIR_BUDGET;
            if ((op->op == XR_XIR_CALL || op->op == XR_XIR_FUNCTION_REF) && context->module->declarations) {
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
            if ((op->op == XR_XIR_CALL || op->op == XR_XIR_FUNCTION_REF) && op->targets[1]) {
                if (op->targets[0] != type_end) return XR_XIR_BAD_STRUCTURE;
                type_end += op->targets[1];
            }
            if ((op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_PRINT || op->op == XR_XIR_PHI) && op->args[1]) {
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
        blocks * (6 * sizeof(uint32_t) + sizeof(uint8_t)) +
        (blocks + 1) * graph->words * sizeof(uint64_t);
    if (bytes > context->remaining.scratch_bytes || bytes > SIZE_MAX || blocks > UINT32_MAX / 2)
        return XR_XIR_BUDGET;
    graph->owner = xr_calloc(function->instruction_count, sizeof(uint32_t));
    graph->queue = xr_calloc((size_t) blocks, sizeof(uint32_t));
    graph->head = xr_calloc((size_t) blocks, sizeof(uint32_t));
    graph->predecessor = xr_calloc((size_t) blocks * 2, sizeof(uint32_t));
    graph->next = xr_calloc((size_t) blocks * 2, sizeof(uint32_t));
    graph->reachable = xr_calloc((size_t) blocks, sizeof(uint8_t));
    graph->dominators = xr_calloc((size_t) blocks * graph->words, sizeof(uint64_t));
    graph->meet = xr_calloc(graph->words, sizeof(uint64_t));
    if (!graph->owner || !graph->queue || !graph->head || !graph->predecessor ||
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
        graph->head[b] = UINT32_MAX;
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
    }
    uint32_t front = 0, count = 1;
    graph->reachable[0] = 1;
    while (front < count) {
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        const XrXirInstruction *op = terminator(function, graph->queue[front++]);
        for (uint32_t i = 0; i < op_rules[op->op].edges; ++i) {
            uint32_t target = op->targets[i];
            if (!graph->reachable[target]) {
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
        function->instructions[definition].type == XR_XIR_UNIT)
        return XR_XIR_BAD_VALUE;
    if (function->instructions[definition].type != expected)
        return XR_XIR_BAD_TYPE;
    uint32_t from = graph->owner[definition], to = graph->owner[instruction];
    if (from == to)
        return definition < instruction ? XR_XIR_OK : XR_XIR_BAD_DOMINANCE;
    uint64_t bits = graph->dominators[(size_t) to * graph->words + from / 64];
    return (bits & (UINT64_C(1) << (from % 64))) ? XR_XIR_OK : XR_XIR_BAD_DOMINANCE;
}

static bool local_place(XrXirOp op) {
    return op == XR_XIR_LOCAL_NEW || op == XR_XIR_SCALAR_LOCAL_NEW || op == XR_XIR_OWNED_LOCAL_NEW;
}
static bool local_access(XrXirOp op) {
    return op == XR_XIR_LOCAL_READ || op == XR_XIR_LOCAL_WRITE ||
        op == XR_XIR_SCALAR_LOCAL_READ || op == XR_XIR_SCALAR_LOCAL_WRITE ||
        op == XR_XIR_OWNED_LOCAL_READ || op == XR_XIR_OWNED_LOCAL_WRITE;
}
static bool local_write(XrXirOp op) {
    return op == XR_XIR_LOCAL_WRITE || op == XR_XIR_SCALAR_LOCAL_WRITE || op == XR_XIR_OWNED_LOCAL_WRITE;
}
static XrXirStatus local_operand(const XrXirFunction *function, const XrXirInstruction *op,
                                  uint32_t id, uint32_t operand) {
    bool place = id >= function->parameter_count && id - function->parameter_count < function->instruction_count &&
        local_place(function->instructions[id - function->parameter_count].op);
    if (place != (operand == 0 && local_access(op->op))) return XR_XIR_BAD_VALUE;
    if (place) {
        XrXirType type = function->instructions[id - function->parameter_count].type;
        if ((op->op == XR_XIR_SCALAR_LOCAL_WRITE && !scalar(type)) ||
            (op->op == XR_XIR_OWNED_LOCAL_WRITE && !xr_xir_type_is_owned(type))) return XR_XIR_BAD_TYPE;
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
        XrXirStatus status = local_operand(function, op, value, 0);
        if (status == XR_XIR_OK) status = value_use(function, graph,
            function->blocks[from].first + function->blocks[from].count - 1, value, op->type);
        if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

static XrXirStatus graph_uses(const Graph *graph, const XrXirFunction *function,
                            VerifyContext *context) {
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        context->location.block = graph->owner[i];
        context->location.instruction = i;
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        const XrXirInstruction *op = &function->instructions[i];
        if (op->op == XR_XIR_PHI) {
            XrXirStatus status = phi_uses(graph, function, context, i);
            if (status != XR_XIR_OK) return status;
            continue;
        }
        XrXirType expected = op->type;
        if (op->op == XR_XIR_RETURN)
            expected = function->result;
        else if (op->op == XR_XIR_BRANCH)
            expected = XR_XIR_BOOL;
        else if (op->op == XR_XIR_EQ_I64 || op->op == XR_XIR_LT_I64 ||
                 (op->op >= XR_XIR_NE_I64 && op->op <= XR_XIR_GE_I64))
            expected = XR_XIR_I64;
        if (op->op == XR_XIR_THROW)
            expected = XR_XIR_I64;
        if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_STORE)
            expected = context->module->declarations->slots[op->immediate].type;
        if (op->op == XR_XIR_ATOMIC_I64_NEW) expected = XR_XIR_I64;
        if (op->op == XR_XIR_WRITE_STREAM) expected = XR_XIR_STRING;
        if (op->op == XR_XIR_ATOMIC_I64_LOAD || op->op == XR_XIR_ATOMIC_I64_FETCH_ADD)
            expected = XR_XIR_ATOMIC_I64;
        if (local_write(op->op)) {
            if (op->args[0] < function->parameter_count ||
                op->args[0] - function->parameter_count >= function->instruction_count) return XR_XIR_BAD_VALUE;
            expected = function->instructions[op->args[0] - function->parameter_count].type;
        }
        const XrXirCallableSignature *indirect = NULL;
        if (op->op == XR_XIR_CALL_INDIRECT) {
            uint32_t callee = (uint32_t) op->immediate;
            XrXirType type = xr_xir_operand_type(function, callee);
            indirect = xr_xir_callable_signature(context->module->callables, type);
            XrXirStatus status = local_operand(function, op, callee, 0);
            if (status == XR_XIR_OK) status = value_use(function, graph, i, callee, type);
            if (status != XR_XIR_OK) return status;
        }
        uint32_t count = operand_count(function, op, context->module);
        if (!spend(&context->remaining.work, count)) return XR_XIR_BUDGET;
        for (uint32_t a = 0; a < count; ++a) {
            XrXirType operand_type = indirect ? indirect->parameters[a].type : expected;
            if (op->op == XR_XIR_CALL) {
                uint32_t operand = function->operands[op->args[0] + a];
                if (operand >= (uint64_t) function->parameter_count + function->instruction_count) return XR_XIR_BAD_VALUE;
                operand_type = xr_xir_operand_type(function, operand);
                XrXirStatus match = xr_xir_call_type_matches(context->module, context->location.function, op,
                    context->module->functions[op->immediate].parameters[a], operand_type, &context->remaining);
                if (match != XR_XIR_OK) return match;
            }
            if (op->op == XR_XIR_ATOMIC_I64_FETCH_ADD && a == 1) operand_type = XR_XIR_I64;
            if (op->op == XR_XIR_OUTPUT || op->op == XR_XIR_PRINT) {
                uint32_t id = op->op == XR_XIR_PRINT ? function->operands[op->args[0] + a] : op->args[a];
                if (id >= function->parameter_count + function->instruction_count) return XR_XIR_BAD_VALUE;
                operand_type = id < function->parameter_count ? function->parameters[id] :
                    function->instructions[id - function->parameter_count].type;
                if (operand_type == XR_XIR_UNIT) return XR_XIR_BAD_VALUE;
                if (!scalar(operand_type) && operand_type != XR_XIR_STRING) return XR_XIR_BAD_TYPE;
            }
            uint32_t id = op->op == XR_XIR_CALL || op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_PRINT ?
                function->operands[op->args[0] + a] : op->args[a];
            XrXirStatus status = local_operand(function, op, id, a);
            if (status == XR_XIR_OK) status = value_use(function, graph, i, id, operand_type);
            if (status != XR_XIR_OK)
                return status;
        }
    }
    return XR_XIR_OK;
}

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
        status = graph_uses(&graph, function, context);
    graph_free(&graph);
    return status;
}

XrXirStatus xr_xir_verify(const XrXirModule *module, const XrXirBudget *budget,
                        XrXirDiagnostic *diagnostic) {
    VerifyContext context = {budget ? *budget : xr_xir_default_budget(),
                             {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX}, module};
    XrXirStatus status = XR_XIR_OK;
    if (!module || !module->functions || !module->function_count)
        status = XR_XIR_BAD_STRUCTURE;
    else if (module->stage != XR_XIR_BUILT && module->stage != XR_XIR_CHECKED &&
             module->stage != XR_XIR_LOWERED)
        status = XR_XIR_BAD_STAGE;
    else if (module->function_count > context.remaining.functions ||
             !spend(&context.remaining.metadata_bytes, sizeof(XrXirArtifact)))
        status = XR_XIR_BUDGET;
    if (status == XR_XIR_OK) {
        status = xr_xir_callable_types_verify(module->callables, &context.remaining);
    }
    if (status == XR_XIR_OK) {
        if (module->callables && (!module->generics || module->stage == XR_XIR_LOWERED))
            for (uint32_t t = 0; t < module->callables->count; ++t)
                if (module->callables->signatures[t].parameter_span) status = XR_XIR_BAD_TYPE;
        if (status == XR_XIR_OK) status = xr_xir_generics_verify(module, &context.remaining);
    }
    if (status == XR_XIR_OK) {
        status = xr_xir_declarations_verify(module->declarations, module->function_count,
            &context.remaining.metadata_bytes, &context.remaining.work);
    }
    if (status == XR_XIR_OK && module->declarations) {
        const XrXirDeclarations *d = module->declarations;
        for (uint32_t s = 0; s < d->slot_count; ++s) {
            const XrXirSlot *slot = &d->slots[s];
            if (xr_xir_type_is_callable(slot->type) && (!xr_xir_callable_signature(module->callables, slot->type) ||
                xr_xir_callable_span(module->callables, slot->type) || slot->module != d->root_module)) status = XR_XIR_BAD_TYPE;
        }
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
            context.location = (XrXirDiagnostic) {XR_XIR_OK, f, UINT32_MAX, UINT32_MAX};
            if (!spend(&context.remaining.work, 1)) {
                status = XR_XIR_BUDGET;
                break;
            }
            status = verify_function(&module->functions[f], module->stage, &context);
            if (status != XR_XIR_OK)
                break;
        }
    }
    context.location.status = status;
    if (diagnostic)
        *diagnostic = context.location;
    return status;
}
