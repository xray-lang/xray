/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_view_cells.inc.c - Physical binding demand through borrowed windows
 */
typedef struct LowerViewCellLink { uint32_t value; size_t next; } LowerViewCellLink;
typedef struct LowerViewCellGraph {
    size_t *heads;
    LowerViewCellLink *links;
    uint32_t *queue, *seen;
    uint8_t *without;
    size_t count, capacity, extent;
} LowerViewCellGraph;

static bool lower_view_cell_link(LowerFunction *f, LowerViewCellGraph *graph, uint32_t parent, uint32_t value) {
    void *grown = lower_view_reserve(f->owner, graph->links, graph->count, &graph->capacity, sizeof(*graph->links));
    if (!grown) return false;
    graph->links = grown;
    graph->links[graph->count] = (LowerViewCellLink){value,graph->heads[parent]};
    graph->heads[parent] = graph->count++; return true;
}
static bool lower_view_cell_graph(LowerFunction *f, LowerViewCellGraph *graph) {
    const XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1];
    graph->extent = function->value_count;
    if (graph->extent == SIZE_MAX) return lower_unsupported(f->owner, "Slice binding demand capacity");
    ++graph->extent;
    graph->heads = lower_alloc(f->owner, graph->extent, sizeof(*graph->heads));
    graph->queue = lower_alloc(f->owner, graph->extent, sizeof(*graph->queue));
    graph->seen = lower_alloc(f->owner, graph->extent, sizeof(*graph->seen));
    graph->without = lower_alloc(f->owner, f->source->block_count, 1);
    if (!graph->heads || !graph->queue || !graph->seen || !graph->without) return false;
    for (size_t n = 0; n < graph->extent; ++n) graph->heads[n] = SIZE_MAX;
    for (uint32_t n = 0; n < function->value_count; ++n) {
        const XrLirInstructionView *op = xr_lir_internal_value_definition(function, n + 1);
        XrLirSliceIntrinsic role;
        if (op && (op->op == XR_LIR_ARRAY_BORROW || (op->op == XR_LIR_CALL_INTRINSIC &&
            xr_lir_slice_intrinsic_view(f->owner->module, op->intrinsic, &role) == XR_LIR_OK &&
            (role.operation == XR_LIR_SLICE_OP_NEW || role.operation == XR_LIR_SLICE_OP_BIND || role.operation == XR_LIR_SLICE_OP_REBORROW))))
            if (!lower_view_cell_link(f, graph, op->operands[0], op->result)) return false;
    }
    for (uint32_t b = 0; b < f->source->block_count; ++b) if (f->reachable[b]) {
        f->owner->block = b;
        f->owner->instruction = f->source->blocks[b].first + f->source->blocks[b].count - 1;
        if (f->cell_count) memcpy(f->current, &f->states[(size_t)b * f->cell_count],
            (size_t)f->cell_count * sizeof(*f->current));
        const XrXirInstruction *term = lower_term(f->owner, f->source, b);
        if (!term) return false;
        for (uint32_t e = 0; e < lower_edge_count(term); ++e) {
            XrLirEdge edge = {0};
            if (!lower_edge(f, b, term->targets[e], &edge)) { xr_free((void *)edge.arguments); return false; }
            bool good = true;
            for (uint32_t a = 0; a < edge.argument_count && good; ++a) {
                XrLirValue source = edge.arguments[a], target = {0};
                if (!xr_lir_internal_slice_type(f->owner->module, function->values[source.id - 1].type)) continue;
                good = lower_build(f->owner, xr_lir_block_parameter(f->owner->module, edge.target, a, &target)) &&
                    lower_view_cell_link(f, graph, source.id, target.id);
            }
            xr_free((void *)edge.arguments); if (!good) return false;
        }
    }
    return true;
}

/* This graph extends physical storage demand only. Read and mutation rights
 * still come from the checked loan verifier, never from this reachability. */
static bool lower_view_cell_demand(LowerFunction *f, LowerViewCellGraph *graph, uint32_t owner) {
    const XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1];
    uint32_t root = f->array_cells.owners[owner].id;
    size_t head = 0, tail = 1; uint32_t generation = owner + 1; bool borrowed = false;
    graph->queue[0] = root; graph->seen[root] = generation;
    while (head < tail) {
        uint32_t value = graph->queue[head++];
        borrowed |= xr_lir_internal_slice_type(f->owner->module, function->values[value - 1].type);
        for (size_t edge = graph->heads[value]; edge != SIZE_MAX; edge = graph->links[edge].next) {
            uint32_t child = graph->links[edge].value;
            if (graph->seen[child] != generation) { graph->seen[child] = generation; graph->queue[tail++] = child; }
        }
    }
    if (!borrowed) return true;
    uint32_t definition_block = function->values[root - 1].block, source_block = UINT32_MAX;
    for (uint32_t b = 0; b < f->source->block_count; ++b)
        if (f->blocks[b].id == definition_block) { source_block = b; break; }
    if (source_block == UINT32_MAX) return lower_unsupported(f->owner, "Slice source binding block");
    memset(graph->without, 0, f->source->block_count); head = tail = 0;
    if (source_block) { f->stack[tail++] = 0; graph->without[0] = 1; }
    while (head < tail) {
        uint32_t b = f->stack[head++]; const XrXirInstruction *term = lower_term(f->owner, f->source, b);
        if (!term) return false;
        for (uint32_t e = 0; e < lower_edge_count(term); ++e) {
            uint32_t target = term->targets[e];
            if (target != source_block && !graph->without[target]) { graph->without[target] = 1; f->stack[tail++] = target; }
        }
    }
    for (uint32_t b = 0; b < f->source->block_count; ++b) if (f->reachable[b]) {
        const XrLirBlockData *block = &function->blocks[f->blocks[b].id - 1]; bool needed = false;
        for (uint32_t p = 0; p < block->parameter_count; ++p) needed |= graph->seen[block->parameters[p]] == generation;
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *op = &block->instructions[i].view;
            for (uint32_t a = 0; a < op->operand_count; ++a) needed |= graph->seen[op->operands[a]] == generation;
            if (op->op == XR_LIR_SLICE_CLEANUP && !graph->without[b] &&
                (b != source_block || i > function->values[root - 1].instruction)) needed |= graph->seen[(uint32_t)op->bits] == generation;
        }
        size_t at = (size_t)b * f->array_cells.count + owner;
        if (needed && !f->array_cells.define[at]) f->array_cells.use[at] = 1;
    }
    return true;
}
static bool lower_view_cell_uses(LowerFunction *f) {
    const XrLirFunctionData *function = &f->owner->module->functions[f->function.id - 1]; bool needed = false;
    for (uint32_t n = 0; n < function->value_count; ++n) needed |= xr_lir_internal_slice_type(f->owner->module, function->values[n].type);
    if (!needed) return true;
    LowerViewCellGraph graph = {0}; bool good = lower_view_cell_graph(f, &graph);
    for (uint32_t owner = 0; owner < f->array_cells.count && good; ++owner) good = lower_view_cell_demand(f, &graph, owner);
    xr_free(graph.heads); xr_free(graph.links); xr_free(graph.queue); xr_free(graph.seen); xr_free(graph.without);
    return good;
}
