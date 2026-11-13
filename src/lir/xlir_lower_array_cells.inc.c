/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_array_cells.inc.c - Retire physical Cell identities after their actual CFG uses
 *
 * KEY CONCEPT:
 *   A synchronous borrower observes the original binding. Its physical owner
 *   remains at the original constructor and retires only on the selected path.
 */
static bool lower_physical_cell(XrLirType type) {
    return type == XR_LIR_ARRAY_CELL || type == XR_LIR_MAP_STRING_I64_CELL;
}
static bool lower_array_cells_inventory(LowerFunction *f) {
    Lower *c = f->owner;
    const XrLirFunctionData *function = &c->module->functions[f->function.id - 1];
    LowerArrayCells *cells = &f->array_cells;
    for (uint32_t v = 0; v < function->value_count; ++v)
        if (lower_physical_cell(function->values[v].type) && !f->cell_retired[v] &&
            xr_lir_internal_value_definition(function, v + 1)) ++cells->count;
    if (!cells->count) return true;
    cells->owners = lower_alloc(c, cells->count, sizeof(*cells->owners));
    cells->indices = lower_alloc(c, function->value_count, sizeof(*cells->indices));
    size_t pairs = (size_t)f->source->block_count * cells->count;
    if (pairs / cells->count != f->source->block_count) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "physical Cell liveness size overflow"); return false;
    }
    cells->use = lower_alloc(c, pairs, 1);
    cells->define = lower_alloc(c, pairs, 1);
    cells->live = lower_alloc(c, pairs, 1);
    if (!cells->owners || !cells->indices || !cells->use || !cells->define || !cells->live) return false;
    uint32_t count = 0;
    for (uint32_t v = 0; v < function->value_count; ++v)
        if (lower_physical_cell(function->values[v].type) && !f->cell_retired[v] &&
            xr_lir_internal_value_definition(function, v + 1)) {
            cells->owners[count] = (XrLirValue){c->module, f->function.id, v + 1};
            cells->indices[v] = ++count;
        }
    XR_DCHECK(count == cells->count, "physical Cell inventory keeps one owner per constructor");
    for (uint32_t v = 0; v < function->value_count; ++v) {
        const XrLirInstructionView *op = xr_lir_internal_value_definition(function, v + 1);
        if (op && (op->op == XR_LIR_ARRAY_BORROW || op->op == XR_LIR_MAP_BORROW))
            cells->indices[v] = cells->indices[op->operands[0] - 1];
    }
    for (uint32_t b = 0; b < f->source->block_count; ++b) if (f->reachable[b]) {
        const XrLirBlockData *block = &function->blocks[f->blocks[b].id - 1];
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *op = &block->instructions[i].view;
            for (uint32_t a = 0; a < op->operand_count; ++a) {
                uint32_t owner = cells->indices[op->operands[a] - 1];
                if (owner) {
                    size_t at = (size_t)b * count + owner - 1;
                    if (!cells->define[at]) cells->use[at] = 1;
                }
            }
            uint32_t owner = op->result ? cells->indices[op->result - 1] : 0;
            if (owner && op->op == XR_LIR_CELL_NEW)
                cells->define[(size_t)b * count + owner - 1] = 1;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t n = f->ordered; n; --n) {
            uint32_t b = f->order[n - 1];
            const XrXirInstruction *term = lower_term(c, f->source, b);
            if (!term) return false;
            for (uint32_t owner = 0; owner < count; ++owner) {
                size_t at = (size_t)b * count + owner;
                bool live_in = cells->use[at] != 0;
                if (!cells->define[at])
                    for (uint32_t e = 0; e < lower_edge_count(term); ++e)
                        live_in |= cells->live[(size_t)term->targets[e] * count + owner] != 0;
                if (live_in && !cells->live[at]) { cells->live[at] = 1; changed = true; }
            }
        }
    }
    return true;
}

static bool lower_array_cell_drop_edge(const LowerFunction *f, uint32_t block,
    const XrXirInstruction *term, uint32_t edge, uint32_t owner) {
    const LowerArrayCells *cells = &f->array_cells;
    size_t at = (size_t)block * cells->count + owner;
    if (!cells->live[at] && !cells->define[at]) return false;
    bool any = false;
    for (uint32_t e = 0; e < lower_edge_count(term); ++e)
        any |= cells->live[(size_t)term->targets[e] * cells->count + owner] != 0;
    if (edge == UINT32_MAX) return !any;
    return any && !cells->live[(size_t)term->targets[edge] * cells->count + owner];
}
static bool lower_array_cells_edge(const LowerFunction *f, uint32_t block, uint32_t edge) {
    if (!f->array_cells.count) return false;
    const XrXirInstruction *term = lower_term(f->owner, f->source, block);
    if (!term) return false;
    for (uint32_t owner = 0; owner < f->array_cells.count; ++owner)
        if (lower_array_cell_drop_edge(f, block, term, edge, owner)) return true;
    return false;
}
static bool lower_array_cells_release(LowerFunction *f, uint32_t from,
    const XrLirInstruction *term, uint32_t edge, XrLirBlock block) {
    if (!f->array_cells.count) return true;
    const XrXirInstruction *source = lower_term(f->owner, f->source, from);
    if (!source) return false;
    for (uint32_t n = f->array_cells.count; n; --n) {
        uint32_t owner = n - 1;
        if (!lower_array_cell_drop_edge(f, from, source, edge, owner)) continue;
        XrLirInstruction drop = {.op = lower_value_type(f, f->array_cells.owners[owner]) == XR_LIR_MAP_STRING_I64_CELL ?
            XR_LIR_MAP_CELL_DROP : XR_LIR_ARRAY_CELL_DROP, .type = XR_LIR_UNIT,
            .operands = &f->array_cells.owners[owner], .operand_count = 1, .source = term->source};
        if (!lower_build(f->owner, xr_lir_emit_effect(f->owner->module, block, &drop))) return false;
    }
    return true;
}
