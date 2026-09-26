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

#include "xxir.h"
#include "../base/xmalloc.h"
#include <limits.h>

typedef enum ResultRule { RULE_UNIT, RULE_BOOL, RULE_I64, RULE_SCALAR } ResultRule;
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

static uint32_t operand_count(const XrXirFunction *function, const XrXirInstruction *op) {
    return op->op == XR_XIR_RETURN ? (function->result != XR_XIR_UNIT) : op_rules[op->op].operands;
}

static XrXirStatus instruction_shape(const XrXirFunction *function,
                                    const XrXirInstruction *op, XrXirStage stage) {
    if (op->op <= XR_XIR_INVALID || op->op >= XR_XIR_OP_COUNT)
        return XR_XIR_BAD_STRUCTURE;
    const OpRule *rule = &op_rules[op->op];
    if (!(rule->stages & stage))
        return XR_XIR_BAD_STAGE;
    if ((rule->result == RULE_UNIT && op->type != XR_XIR_UNIT) ||
        (rule->result == RULE_BOOL && op->type != XR_XIR_BOOL) ||
        (rule->result == RULE_I64 && op->type != XR_XIR_I64) ||
        (rule->result == RULE_SCALAR && !scalar(op->type)))
        return XR_XIR_BAD_TYPE;
    uint32_t operands = operand_count(function, op);
    for (uint32_t i = operands; i < 2; ++i)
        if (op->args[i])
            return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < rule->edges; ++i)
        if (!op->targets[i] || op->targets[i] >= function->block_count)
            return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = rule->edges; i < 2; ++i)
        if (op->targets[i])
            return XR_XIR_BAD_STRUCTURE;
    if (op->op == XR_XIR_CONST_BOOL) {
        if (op->immediate != 0 && op->immediate != 1)
            return XR_XIR_BAD_TYPE;
    } else if (op->op != XR_XIR_CONST_I64 && op->immediate) {
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
        (uint64_t) function->instruction_count * sizeof(*function->instructions);
    if (!spend(&context->remaining.metadata_bytes, bytes) || bytes > SIZE_MAX)
        return XR_XIR_BUDGET;
    if (!function->name || !function->name_length || !function->blocks ||
        !function->block_count || !function->instructions || !function->instruction_count ||
        (function->parameter_count && !function->parameters) ||
        function->parameter_count > UINT32_MAX - function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (function->result != XR_XIR_UNIT && !scalar(function->result))
        return XR_XIR_BAD_TYPE;
    if (!spend(&context->remaining.work, function->name_length))
        return XR_XIR_BUDGET;
    if (memchr(function->name, 0, function->name_length))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < function->parameter_count; ++p) {
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        if (!scalar(function->parameters[p]))
            return XR_XIR_BAD_TYPE;
    }
    uint32_t end = 0;
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
            XrXirStatus status = instruction_shape(function, op, stage);
            if (status != XR_XIR_OK)
                return status;
            if (op_rules[op->op].terminal != (i == end - 1))
                return XR_XIR_BAD_STRUCTURE;
        }
    }
    return end == function->instruction_count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
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

static XrXirStatus graph_uses(const Graph *graph, const XrXirFunction *function,
                            VerifyContext *context) {
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        context->location.block = graph->owner[i];
        context->location.instruction = i;
        if (!spend(&context->remaining.work, 1))
            return XR_XIR_BUDGET;
        const XrXirInstruction *op = &function->instructions[i];
        XrXirType expected = op->type;
        if (op->op == XR_XIR_RETURN)
            expected = function->result;
        else if (op->op == XR_XIR_BRANCH)
            expected = XR_XIR_BOOL;
        else if (op->op == XR_XIR_EQ_I64)
            expected = XR_XIR_I64;
        uint32_t count = operand_count(function, op);
        for (uint32_t a = 0; a < count; ++a) {
            XrXirStatus status = value_use(function, graph, i, op->args[a], expected);
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
                             {XR_XIR_OK, UINT32_MAX, UINT32_MAX, UINT32_MAX}};
    XrXirStatus status = XR_XIR_OK;
    if (!module || !module->functions || !module->function_count)
        status = XR_XIR_BAD_STRUCTURE;
    else if (module->stage != XR_XIR_BUILT && module->stage != XR_XIR_CHECKED &&
             module->stage != XR_XIR_LOWERED)
        status = XR_XIR_BAD_STAGE;
    else if (module->function_count > context.remaining.functions ||
             !spend(&context.remaining.metadata_bytes, sizeof(*module)))
        status = XR_XIR_BUDGET;
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
