/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_cell_flow.inc.c - Physical Array Cell availability through actual CFG edges
 *
 * KEY CONCEPT:
 *   A constructor creates one active instance. Synchronous parameters borrow
 *   that instance, and every normal exit or reconstruction consumes its owner.
 */
enum { LIR_ARRAY_CELL_EMPTY = 1, LIR_ARRAY_CELL_OWNED = 2 };
typedef struct LirArrayCellFlow {
    const XrLirFunctionData *function;
    const LirGraph *graph;
    uint32_t *owners;
    uint8_t *out, *current;
    uint32_t count;
} LirArrayCellFlow;

static void lir_array_cell_input(LirArrayCellFlow *flow, uint32_t block) {
    if (!flow->count) return;
    const LirGraph *g = flow->graph;
    memset(flow->current, block == g->entry ? LIR_ARRAY_CELL_EMPTY : 0, flow->count);
    for (uint32_t p = g->offsets[block]; p < g->offsets[block + 1]; ++p) {
        const uint8_t *out = &flow->out[(size_t)g->predecessors[p] * flow->count];
        for (uint32_t c = 0; c < flow->count; ++c) flow->current[c] |= out[c];
    }
}
static void lir_array_cell_transfer(LirArrayCellFlow *flow, uint32_t block) {
    const XrLirBlockData *body = &flow->function->blocks[block];
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        const XrLirInstructionView *v = &body->instructions[i].view;
        if (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_ARRAY_CELL)
            flow->current[flow->owners[v->result - 1] - 1] = LIR_ARRAY_CELL_OWNED;
        else if (v->op == XR_LIR_ARRAY_CELL_DROP && flow->owners[v->operands[0] - 1])
            flow->current[flow->owners[v->operands[0] - 1] - 1] = LIR_ARRAY_CELL_EMPTY;
    }
}
static XrLirStatus lir_array_cell_block(LirArrayCellFlow *flow, uint32_t function,
    uint32_t block, XrLirDiagnostic *d) {
    const XrLirFunctionData *f = flow->function;
    const XrLirBlockData *body = &f->blocks[block];
    lir_array_cell_input(flow, block);
    for (uint32_t i = 0; i < body->instruction_count; ++i) {
        const XrLirInstructionView *v = &body->instructions[i].view;
        for (uint32_t a = 0; a < v->operand_count; ++a) {
            uint32_t id = v->operands[a];
            XrLirType type = f->values[id - 1].type;
            if (type != XR_LIR_ARRAY_CELL && type != XR_LIR_ARRAY_REF) continue;
            uint32_t owner = flow->owners[id - 1];
            if (owner && flow->current[owner - 1] != LIR_ARRAY_CELL_OWNED)
                return xr_lir_internal_error(d, "Managed Cell use follows release or precedes initialization",
                    function, block + 1, i + 1, &v->source);
            bool allowed = type == XR_LIR_ARRAY_REF ?
                v->op == XR_LIR_ARRAY_REF_READ || v->op == XR_LIR_ARRAY_REF_WRITE ||
                v->op == XR_LIR_CALL || v->op == XR_LIR_CALL_INTRINSIC :
                v->op == XR_LIR_CELL_READ || v->op == XR_LIR_CELL_WRITE || v->op == XR_LIR_CALL ||
                v->op == XR_LIR_CALL_INTRINSIC || v->op == XR_LIR_ARRAY_CELL_DROP || v->op == XR_LIR_ARRAY_BORROW;
            if (!allowed)
                return xr_lir_internal_error(d, "Managed Cell handle cannot escape its activation",
                    function, block + 1, i + 1, &v->source);
            if (v->op == XR_LIR_ARRAY_CELL_DROP) {
                if (!owner)
                    return xr_lir_internal_error(d, "incoming Managed Cell borrow cannot be released",
                        function, block + 1, i + 1, &v->source);
                flow->current[owner - 1] = LIR_ARRAY_CELL_EMPTY;
            }
        }
        if (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_ARRAY_CELL) {
            uint32_t owner = flow->owners[v->result - 1] - 1;
            if (flow->current[owner] != LIR_ARRAY_CELL_EMPTY)
                return xr_lir_internal_error(d, "Array Cell reconstruction requires an empty lifetime",
                    function, block + 1, i + 1, &v->source);
            flow->current[owner] = LIR_ARRAY_CELL_OWNED;
        }
        for (uint32_t e = 0; e < v->edge_count; ++e)
            for (uint32_t a = 0; a < v->edges[e].argument_count; ++a)
                if (f->values[v->edges[e].arguments[a] - 1].type == XR_LIR_ARRAY_CELL ||
                    f->values[v->edges[e].arguments[a] - 1].type == XR_LIR_ARRAY_REF)
                    return xr_lir_internal_error(d, "Managed Cell handle cannot transfer on an edge",
                        function, block + 1, i + 1, &v->source);
        if (v->op == XR_LIR_RET)
            for (uint32_t c = 0; c < flow->count; ++c)
                if (flow->current[c] != LIR_ARRAY_CELL_EMPTY)
                    return xr_lir_internal_error(d, "Array Cell owner requires release on each normal exit",
                        function, block + 1, i + 1, &v->source);
    }
    return XR_LIR_OK;
}

static XrLirStatus lir_array_cells(const XrLirFunctionData *f, uint32_t function,
    const LirGraph *g, XrLirDiagnostic *d) {
    uint32_t count = 0;
    for (uint32_t v = 0; v < f->value_count; ++v) {
        if (f->values[v].type != XR_LIR_ARRAY_CELL && f->values[v].type != XR_LIR_ARRAY_REF) continue;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v + 1);
        if (f->values[v].type == XR_LIR_ARRAY_CELL && definition && definition->op == XR_LIR_CELL_NEW) ++count;
        else if (f->values[v].type == XR_LIR_ARRAY_REF && definition && definition->op == XR_LIR_ARRAY_BORROW)
            continue;
        else if (!f->values[v].parameter || f->values[v].block)
            return xr_lir_internal_error(d, "Array Cell requires a real constructor or incoming borrow",
                function, f->values[v].block, 0, definition ? &definition->source : NULL);
    }
    size_t owner_bytes = (size_t)f->value_count * sizeof(uint32_t);
    size_t pairs = (size_t)f->block_count * count;
    if ((f->value_count && owner_bytes / sizeof(uint32_t) != f->value_count) ||
        (count && pairs / count != f->block_count))
        return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "Array Cell flow allocation size overflow");
    LirArrayCellFlow flow = {.function = f, .graph = g, .count = count};
    flow.owners = f->value_count ? xr_malloc(owner_bytes) : NULL;
    flow.out = pairs ? xr_malloc(pairs) : NULL;
    flow.current = count ? xr_malloc(count) : NULL;
    if ((f->value_count && !flow.owners) || (pairs && !flow.out) || (count && !flow.current)) {
        xr_free(flow.current); xr_free(flow.out); xr_free(flow.owners);
        return lir_status_error(d, XR_LIR_OUT_OF_MEMORY, "Array Cell flow allocation failed");
    }
    if (owner_bytes) memset(flow.owners, 0, owner_bytes);
    if (pairs) memset(flow.out, 0, pairs);
    uint32_t owner = 0;
    for (uint32_t v = 0; v < f->value_count; ++v) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v + 1);
        if (definition && definition->op == XR_LIR_CELL_NEW && definition->type == XR_LIR_ARRAY_CELL)
            flow.owners[v] = ++owner;
    }
    for (uint32_t v = 0; v < f->value_count; ++v) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, v + 1);
        if (definition && definition->op == XR_LIR_ARRAY_BORROW)
            flow.owners[v] = flow.owners[definition->operands[0] - 1];
    }
    XR_DCHECK(owner == count, "each Array Cell constructor has one flow owner");
    bool changed = count != 0;
    while (changed) {
        changed = false;
        for (uint32_t r = 0; r < g->count; ++r) {
            uint32_t block = g->order[r];
            lir_array_cell_input(&flow, block);
            lir_array_cell_transfer(&flow, block);
            uint8_t *out = &flow.out[(size_t)block * count];
            if (memcmp(out, flow.current, count)) { memcpy(out, flow.current, count); changed = true; }
        }
    }
    XrLirStatus status = XR_LIR_OK;
    for (uint32_t b = 0; b < f->block_count && status == XR_LIR_OK; ++b)
        status = lir_array_cell_block(&flow, function, b, d);
    xr_free(flow.current); xr_free(flow.out); xr_free(flow.owners);
    return status;
}
