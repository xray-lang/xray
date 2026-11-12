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

/* Registration only advances a lexical frontier and transports existing
 * operands. Cleanup invocation and its panic outcomes belong to the exit. */
static inline bool xr_xir_instruction_has_panic_port(XrXirOp op) {
    switch (op) {
    case XR_XIR_CLEANUP_REGISTER:
    case XR_XIR_CONST_BOOL: case XR_XIR_CONST_INT:
    case XR_XIR_FUNCTION_REF: case XR_XIR_FUNCTION_WEAKEN:
    case XR_XIR_COPY: case XR_XIR_SCALAR_COPY: case XR_XIR_OWNED_RETAIN:
        return false;
    default: return true;
    }
}

static inline bool xr_xir_instruction_cleanup_exit(XrXirOp op) {
    return op == XR_XIR_CLEANUP_LEAVE || op == XR_XIR_CLEANUP_ERROR ||
        op == XR_XIR_RETURN || op == XR_XIR_THROW;
}

/* A cleanup panic is raised at its registration site. The exit that invokes
 * it may be inside a different lexical panic handler. */
static inline XrXirStatus xr_xir_cleanup_frontier(const XrXirFunction *function,
    uint32_t frontier, uint32_t *parent, uint32_t *handler) {
    if (!function->blocks || !function->block_count || !frontier ||
        frontier > function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    /* Streaming observers retain the validated block headers and their own
     * operation records. A full body additionally checks the registration. */
    if (function->instructions && function->instructions[frontier - 1].op != XR_XIR_CLEANUP_REGISTER)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t low = 0, high = function->block_count;
    while (low + 1 < high) {
        uint32_t middle = low + (high - low) / 2;
        if (function->blocks[middle].first < frontier) low = middle;
        else high = middle;
    }
    *parent = function->blocks[low].frontier;
    *handler = function->blocks[low].panic;
    return *parent < frontier && *handler < function->block_count ? XR_XIR_OK : XR_XIR_BAD_STRUCTURE;
}

static inline XrXirStatus xr_xir_cleanup_panic_edges(const XirInstructionFlowPoint *point,
    bool external, XirInstructionFlowVisitor visit, void *context) {
    const XrXirFunction *function = point->function;
    const XrXirInstruction *op = point->op;
    if (external && function->instruction_count > UINT32_MAX - 3) return XR_XIR_OVERFLOW;
    uint32_t target = 0;
    if (op->op == XR_XIR_CLEANUP_LEAVE || op->op == XR_XIR_CLEANUP_ERROR) {
        if (op->immediate < 0 || (uint64_t)op->immediate > function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        target = (uint32_t)op->immediate;
    }
    uint32_t frontier = function->blocks[point->block].frontier;
    while (frontier != target) {
        uint32_t parent = 0, handler = 0;
        XrXirStatus status = xr_xir_cleanup_frontier(function, frontier, &parent, &handler);
        if (status != XR_XIR_OK) return status;
        if ((handler == 0) == external) {
            XirInstructionFlowEdge edge = {point->instruction,
                external ? function->instruction_count + 2 : function->blocks[handler].first,
                point->block, external ? UINT32_MAX : handler, handler, XIR_INSTRUCTION_FLOW_PANIC};
            status = visit(context, &edge);
            if (status != XR_XIR_OK) return status;
            if (external) break;
        }
        frontier = parent;
    }
    return XR_XIR_OK;
}

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
    if (status == XR_XIR_OK && (kinds & XIR_INSTRUCTION_FLOW_PANIC) && xr_xir_instruction_cleanup_exit(op->op))
        return xr_xir_cleanup_panic_edges(point, false, visit, context);
    if (status == XR_XIR_OK && (kinds & XIR_INSTRUCTION_FLOW_PANIC) && source->panic &&
        xr_xir_instruction_has_panic_port(op->op)) {
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
