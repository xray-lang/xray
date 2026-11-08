/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instruction_flow.h - Shared structural instruction successors
 *
 * KEY CONCEPT:
 *   Panic successors observe the state before the source instruction.
 *   Consumers retain their own branch filters and transfer functions.
 */
#ifndef XXIR_INSTRUCTION_FLOW_H
#define XXIR_INSTRUCTION_FLOW_H
#include "xxir.h"

typedef enum XirInstructionFlowKind {
    XIR_INSTRUCTION_FLOW_NEXT = 1,
    XIR_INSTRUCTION_FLOW_TARGET = 2,
    XIR_INSTRUCTION_FLOW_PANIC = 4
} XirInstructionFlowKind;
enum { XIR_INSTRUCTION_FLOW_NORMAL = 3, XIR_INSTRUCTION_FLOW_ALL = 7 };

typedef struct XirInstructionFlowEdge {
    uint32_t source, target, source_block, target_block, ordinal;
    XirInstructionFlowKind kind;
} XirInstructionFlowEdge;
typedef XrXirStatus (*XirInstructionFlowVisitor)(void *, const XirInstructionFlowEdge *);
typedef struct XirInstructionFlowPoint {
    const XrXirFunction *function;
    const XrXirInstruction *op;
    uint32_t instruction, block;
} XirInstructionFlowPoint;

/* Detached initialization may name its synthetic instruction-count exit.
 * Admission and consumer-specific reachability remain separate obligations. */
static inline XrXirStatus xr_xir_instruction_flow_point(const XirInstructionFlowPoint *point,
    uint32_t kinds, XirInstructionFlowVisitor visit, void *context) {
    static const struct { uint8_t edges; bool terminal; } rules[XR_XIR_OP_COUNT] = {
        {0, false},
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) {edges, terminal != 0},
#include "xxir_ops.def"
#undef XR_XIR_OP
    };
    if (!point || !point->function || !point->op) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunction *function = point->function;
    const XrXirInstruction *op = point->op;
    uint32_t instruction = point->instruction, block = point->block;
    if (!visit || !function->blocks ||
        instruction >= function->instruction_count || block >= function->block_count ||
        (kinds & ~XIR_INSTRUCTION_FLOW_ALL)) return XR_XIR_BAD_STRUCTURE;
    const XrXirBlock *source = &function->blocks[block];
    if (instruction < source->first || instruction - source->first >= source->count ||
        !op->op || (uint32_t)op->op >= XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = XR_XIR_OK;
    if ((kinds & XIR_INSTRUCTION_FLOW_NEXT) && !rules[op->op].terminal) {
        XirInstructionFlowEdge edge = {instruction, instruction + 1, block, block, 0, XIR_INSTRUCTION_FLOW_NEXT};
        status = visit(context, &edge);
    }
    for (uint32_t e = 0; status == XR_XIR_OK && (kinds & XIR_INSTRUCTION_FLOW_TARGET) &&
        e < rules[op->op].edges; ++e) {
        uint32_t target = op->targets[e];
        if (target >= function->block_count || function->blocks[target].first > function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        XirInstructionFlowEdge edge = {instruction, function->blocks[target].first,
            block, target, e, XIR_INSTRUCTION_FLOW_TARGET};
        status = visit(context, &edge);
    }
    if (status == XR_XIR_OK && (kinds & XIR_INSTRUCTION_FLOW_PANIC) && source->panic) {
        uint32_t target = source->panic;
        if (target >= function->block_count || function->blocks[target].first >= function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        XirInstructionFlowEdge edge = {instruction, function->blocks[target].first,
            block, target, 0, XIR_INSTRUCTION_FLOW_PANIC};
        status = visit(context, &edge);
    }
    return status;
}
static inline XrXirStatus xr_xir_instruction_flow_visit(const XrXirFunction *function,
    uint32_t instruction, uint32_t block, uint32_t kinds,
    XirInstructionFlowVisitor visit, void *context) {
    if (!function || !function->instructions || instruction >= function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    XirInstructionFlowPoint point = {function, &function->instructions[instruction], instruction, block};
    return xr_xir_instruction_flow_point(&point, kinds, visit, context);
}
#endif // XXIR_INSTRUCTION_FLOW_H
