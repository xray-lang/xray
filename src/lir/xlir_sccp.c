/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_sccp.c - Sparse integer facts and executable successor ordinals
 *
 * KEY CONCEPT:
 *   Monotone value facts and executable edges converge before an immutable
 *   projection rebuilds owned SSA. Unknown operations retain their behavior.
 */
#include "xlir_sccp.h"
#include "xlir_rebuild_internal.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

typedef enum { SCCP_UNSEEN, SCCP_CONSTANT, SCCP_OVERDEFINED } SccpKind;
typedef struct {
    SccpKind kind;
    XrLirType type;
    uint64_t bits;
    size_t users;
} SccpCell;
typedef struct { size_t node, next; } SccpUse;
typedef struct {
    uint32_t block, ordinal;
    bool parameter, queued;
} SccpNode;
typedef struct {
    XrLirBlockView view;
    size_t first, incoming;
    bool executable;
} SccpBlock;
typedef struct { XrLirEdgeView view; size_t next; bool executable; } SccpEdge;
typedef struct {
    const XrLirModule *input;
    XrLirFunction function;
    XrLirFunctionView view;
    SccpCell *cells;
    SccpBlock *blocks;
    SccpNode *nodes;
    SccpUse *uses;
    SccpEdge *edges;
    size_t *queue, *edge_queue;
    size_t node_count, use_count, use_filled, edge_capacity;
    size_t head, pending, edge_head, edge_tail;
} Sccp;

static bool sccp_count(size_t *count, uint64_t extra) {
    if (extra > SIZE_MAX - *count) return false;
    *count += (size_t)extra;
    return true;
}
static void *sccp_storage(size_t count, size_t size) {
    return count && count <= SIZE_MAX / size ? xr_calloc(count, size) : NULL;
}
static void sccp_end(void *context) {
    Sccp *c = context;
    xr_free(c->cells); xr_free(c->blocks); xr_free(c->nodes); xr_free(c->uses);
    xr_free(c->edges); xr_free(c->queue); xr_free(c->edge_queue);
    *c = (Sccp){0};
}
static XrLirBlock sccp_block(const Sccp *c, uint32_t id) {
    return (XrLirBlock){c->input, c->function.id, id};
}
static XrLirInstructionView sccp_instruction(const Sccp *c, uint32_t block, uint32_t ordinal) {
    XrLirInstructionView view;
    xr_lir_instruction_view(c->input, sccp_block(c, block), ordinal, &view);
    return view;
}

static XrLirStatus sccp_allocate(Sccp *c) {
    size_t values = c->view.value_count, blocks = c->view.block_count;
    if (!sccp_count(&values, 1) || !sccp_count(&blocks, 1) ||
        !sccp_count(&c->edge_capacity, (uint64_t)c->view.block_count * 2))
        return XR_LIR_OUT_OF_MEMORY;
    c->cells = sccp_storage(values, sizeof(*c->cells));
    c->blocks = sccp_storage(blocks, sizeof(*c->blocks));
    if (!c->cells || !c->blocks) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        SccpBlock *block = &c->blocks[b + 1];
        xr_lir_block_view(c->input, sccp_block(c, b + 1), &block->view);
        block->first = c->node_count;
        if (!sccp_count(&c->node_count, (uint64_t)block->view.parameter_count +
                block->view.instruction_count)) return XR_LIR_OUT_OF_MEMORY;
        for (uint32_t n = 0; n < block->view.instruction_count; ++n) {
            XrLirInstructionView v = sccp_instruction(c, b + 1, n);
            if (!sccp_count(&c->use_count, v.operand_count)) return XR_LIR_OUT_OF_MEMORY;
            for (uint32_t e = 0; e < v.edge_count; ++e)
                if (!sccp_count(&c->use_count, v.edges[e].argument_count))
                    return XR_LIR_OUT_OF_MEMORY;
        }
    }
    size_t uses = c->use_count;
    if (!sccp_count(&uses, 1)) return XR_LIR_OUT_OF_MEMORY;
    c->nodes = sccp_storage(c->node_count, sizeof(*c->nodes));
    c->uses = sccp_storage(uses, sizeof(*c->uses));
    c->edges = sccp_storage(c->edge_capacity, sizeof(*c->edges));
    c->queue = sccp_storage(c->node_count, sizeof(*c->queue));
    c->edge_queue = sccp_storage(c->edge_capacity, sizeof(*c->edge_queue));
    return c->nodes && c->uses && c->edges && c->queue && c->edge_queue ?
        XR_LIR_OK : XR_LIR_OUT_OF_MEMORY;
}

static void sccp_use(Sccp *c, uint32_t value, size_t node) {
    XR_DCHECK(value && value <= c->view.value_count && node < c->node_count,
        "Verified operands identify existing values and nodes");
    XR_DCHECK(c->use_filled < c->use_count, "Use storage was sized from immutable operands");
    size_t index = ++c->use_filled;
    c->uses[index] = (SccpUse){node, c->cells[value].users};
    c->cells[value].users = index;
}
static void sccp_index(Sccp *c) {
    for (uint32_t v = 0; v < c->view.value_count; ++v)
        xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, v + 1},
            &c->cells[v + 1].type);
    for (uint32_t p = 0; p < c->view.parameter_count; ++p)
        c->cells[c->view.parameters[p]].kind = SCCP_OVERDEFINED;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        SccpBlock *block = &c->blocks[b + 1];
        for (uint32_t p = 0; p < block->view.parameter_count; ++p)
            c->nodes[block->first + p] = (SccpNode){b + 1, p, true, false};
        for (uint32_t n = 0; n < block->view.instruction_count; ++n) {
            size_t node = block->first + block->view.parameter_count + n;
            c->nodes[node] = (SccpNode){b + 1, n, false, false};
            XrLirInstructionView v = sccp_instruction(c, b + 1, n);
            for (uint32_t o = 0; o < v.operand_count; ++o) sccp_use(c, v.operands[o], node);
            for (uint32_t e = 0; e < v.edge_count; ++e) {
                size_t index = (size_t)b * 2 + e;
                SccpBlock *target = &c->blocks[v.edges[e].target];
                c->edges[index] = (SccpEdge){v.edges[e], target->incoming, false};
                target->incoming = index + 1;
                for (uint32_t p = 0; p < v.edges[e].argument_count; ++p)
                    sccp_use(c, v.edges[e].arguments[p], target->first + p);
            }
        }
    }
    XR_DCHECK(c->use_filled == c->use_count, "Every immutable operand has one sparse use");
}

static void sccp_enqueue(Sccp *c, size_t node) {
    SccpNode *n = &c->nodes[node];
    if (n->queued || !c->blocks[n->block].executable) return;
    XR_DCHECK(c->pending < c->node_count, "A queued flag bounds pending SSA nodes");
    c->queue[(c->head + c->pending) % c->node_count] = node;
    ++c->pending;
    n->queued = true;
}
static void sccp_activate_block(Sccp *c, uint32_t id) {
    SccpBlock *block = &c->blocks[id];
    if (block->executable) return;
    block->executable = true;
    size_t count = (size_t)block->view.parameter_count + block->view.instruction_count;
    for (size_t n = 0; n < count; ++n) sccp_enqueue(c, block->first + n);
}
static void sccp_activate_edge(Sccp *c, uint32_t block, uint32_t ordinal) {
    size_t index = ((size_t)block - 1) * 2 + ordinal;
    if (c->edges[index].executable) return;
    XR_DCHECK(c->edge_tail < c->edge_capacity, "Each successor ordinal activates at most once");
    c->edges[index].executable = true;
    c->edge_queue[c->edge_tail++] = index;
}
static SccpCell sccp_join(SccpCell a, SccpCell b) {
    if (a.kind == SCCP_UNSEEN) return b;
    if (b.kind == SCCP_UNSEEN) return a;
    if (a.kind == SCCP_OVERDEFINED || b.kind == SCCP_OVERDEFINED ||
        a.type != b.type || a.bits != b.bits) a.kind = SCCP_OVERDEFINED;
    return a;
}
static void sccp_update(Sccp *c, uint32_t id, SccpCell value) {
    SccpCell *old = &c->cells[id];
    value = sccp_join(*old, value);
    if (old->kind == value.kind && (value.kind != SCCP_CONSTANT || old->bits == value.bits)) return;
    old->kind = value.kind;
    old->bits = value.bits;
    for (size_t u = old->users; u; u = c->uses[u].next) sccp_enqueue(c, c->uses[u].node);
}
static void sccp_parameter(Sccp *c, const SccpNode *node) {
    SccpBlock *block = &c->blocks[node->block];
    uint32_t id = block->view.parameters[node->ordinal];
    SccpCell value = {.type = c->cells[id].type};
    for (size_t e = block->incoming; e; e = c->edges[e - 1].next) {
        const SccpEdge *edge = &c->edges[e - 1];
        if (edge->executable) value = sccp_join(value, c->cells[edge->view.arguments[node->ordinal]]);
    }
    sccp_update(c, id, value);
}

static bool sccp_compare(uint64_t a, uint64_t b, XrLirPredicate predicate) {
    uint64_t left = a ^ (UINT64_C(1) << 63), right = b ^ (UINT64_C(1) << 63);
    switch (predicate) {
        case XR_LIR_EQ: return a == b;
        case XR_LIR_NE: return a != b;
        case XR_LIR_LT: return left < right;
        case XR_LIR_LE: return left <= right;
        case XR_LIR_GT: return left > right;
        case XR_LIR_GE: return left >= right;
        default: XR_CHECK(false, "Verified integer comparisons have a predicate"); return false;
    }
}
static SccpCell sccp_fold(const Sccp *c, const XrLirInstructionView *v) {
    SccpCell value = {.kind = SCCP_OVERDEFINED, .type = v->type};
    if (v->op == XR_LIR_CONST && (v->type == XR_LIR_I64 || v->type == XR_LIR_I1)) {
        value.kind = SCCP_CONSTANT; value.bits = v->bits;
        return value;
    }
    bool arithmetic = v->type == XR_LIR_I64 && (v->op == XR_LIR_ADD ||
        v->op == XR_LIR_SUB || v->op == XR_LIR_MUL || v->op == XR_LIR_NEG);
    bool compare = v->op == XR_LIR_ICMP && (c->cells[v->operands[0]].type == XR_LIR_I64 ||
        c->cells[v->operands[0]].type == XR_LIR_I1);
    if (!arithmetic && !compare) return value;
    SccpCell a = c->cells[v->operands[0]];
    SccpCell b = v->operand_count == 2 ? c->cells[v->operands[1]] : a;
    if (a.kind == SCCP_OVERDEFINED || b.kind == SCCP_OVERDEFINED) return value;
    if (a.kind == SCCP_UNSEEN || b.kind == SCCP_UNSEEN) { value.kind = SCCP_UNSEEN; return value; }
    value.kind = SCCP_CONSTANT;
    switch (v->op) {
        case XR_LIR_ADD: value.bits = a.bits + b.bits; break;
        case XR_LIR_SUB: value.bits = a.bits - b.bits; break;
        case XR_LIR_MUL: value.bits = a.bits * b.bits; break;
        case XR_LIR_NEG: value.bits = UINT64_C(0) - a.bits; break;
        case XR_LIR_ICMP: value.bits = sccp_compare(a.bits, b.bits, v->predicate); break;
        default: XR_CHECK(false, "Only admitted integer operations are folded");
    }
    return value;
}
static void sccp_visit(Sccp *c, const SccpNode *node) {
    if (node->parameter) { sccp_parameter(c, node); return; }
    XrLirInstructionView v = sccp_instruction(c, node->block, node->ordinal);
    if (v.result) sccp_update(c, v.result, sccp_fold(c, &v));
    if (v.op == XR_LIR_BR) sccp_activate_edge(c, node->block, 0);
    if (v.op == XR_LIR_CONDBR) {
        SccpCell condition = c->cells[v.operands[0]];
        if (condition.kind == SCCP_CONSTANT) sccp_activate_edge(c, node->block, condition.bits ? 0 : 1);
        else if (condition.kind == SCCP_OVERDEFINED) {
            sccp_activate_edge(c, node->block, 0);
            sccp_activate_edge(c, node->block, 1);
        }
    }
}
static void sccp_propagate(Sccp *c) {
    while (c->pending || c->edge_head < c->edge_tail) {
        while (c->edge_head < c->edge_tail) {
            const SccpEdge *edge = &c->edges[c->edge_queue[c->edge_head++]];
            sccp_activate_block(c, edge->view.target);
            SccpBlock *target = &c->blocks[edge->view.target];
            // Parallel edges must remeet parameters even after the first made the block live.
            for (uint32_t p = 0; p < target->view.parameter_count; ++p)
                sccp_enqueue(c, target->first + p);
        }
        if (!c->pending) continue;
        size_t node = c->queue[c->head];
        c->head = (c->head + 1) % c->node_count;
        --c->pending;
        c->nodes[node].queued = false;
        sccp_visit(c, &c->nodes[node]);
    }
}
static bool sccp_unknown_edges(Sccp *c) {
    bool changed = false;
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        SccpBlock *block = &c->blocks[b + 1];
        if (!block->executable) continue;
        XrLirInstructionView v = sccp_instruction(c, b + 1, block->view.instruction_count - 1);
        if (v.op == XR_LIR_CONDBR && c->cells[v.operands[0]].kind == SCCP_UNSEEN) {
            // Retained fallback edges must not later shrink to a single constant edge.
            sccp_update(c, v.operands[0], (SccpCell){.kind = SCCP_OVERDEFINED, .type = XR_LIR_I1});
            sccp_activate_edge(c, b + 1, 0);
            sccp_activate_edge(c, b + 1, 1);
            changed = true;
        }
    }
    return changed;
}
static XrLirStatus sccp_begin(void *context, const XrLirModule *input, XrLirFunction function) {
    Sccp *c = context;
    XR_DCHECK(!c->cells && !c->blocks, "The preceding function released all analysis scratch");
    c->input = input; c->function = function;
    xr_lir_function_view(input, function, &c->view);
    XrLirStatus status = sccp_allocate(c);
    if (status != XR_LIR_OK) return status;
    sccp_index(c);
    sccp_activate_block(c, c->view.entry);
    do { sccp_propagate(c); } while (sccp_unknown_edges(c));
    return XR_LIR_OK;
}
static bool sccp_keep(void *context, uint32_t block) {
    return ((Sccp *)context)->blocks[block].executable;
}
static void sccp_project(void *context, uint32_t block, uint32_t ordinal,
    XrLirInstructionView *v) {
    Sccp *c = context;
    (void)block; (void)ordinal;
    if (v->result && v->op != XR_LIR_CONST && c->cells[v->result].kind == SCCP_CONSTANT) {
        *v = (XrLirInstructionView){.op = XR_LIR_CONST, .type = v->type,
            .result = v->result, .bits = c->cells[v->result].bits, .source = v->source};
    } else if (v->op == XR_LIR_CONDBR && c->cells[v->operands[0]].kind == SCCP_CONSTANT) {
        XrLirEdgeView edge = v->edges[c->cells[v->operands[0]].bits ? 0 : 1];
        *v = (XrLirInstructionView){.op = XR_LIR_BR, .type = XR_LIR_UNIT,
            .edges = {edge}, .edge_count = 1, .source = v->source};
    }
}

XR_FUNC XrLirStatus xr_lir_sccp(const XrLirModule *input, XrLirModule **output) {
    Sccp c = {0};
    const XrLirCopyPlan plan = {&c, sccp_begin, sccp_keep, sccp_project, sccp_end};
    return xr_lir_internal_copy(input, output, &plan);
}
