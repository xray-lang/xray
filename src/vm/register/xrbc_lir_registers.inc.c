/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_registers.inc.c - Interference-safe block argument coalescing.
 *
 * KEY CONCEPT:
 *   Edge actuals are predecessor uses, while block parameters are simultaneous
 *   definitions. A merged group must respect every member's interference.
 */
typedef struct { uint64_t words[4]; } RbcLirSet;
typedef struct {
    RbcLirSet conflicts[256], members[256];
    uint32_t parent[256], fixed[256];
    RbcLirSet *live;
} RbcLirRegisters;

static void rbc_lir_set_add(RbcLirSet *set, uint32_t value) {
    if (value) set->words[value / 64] |= UINT64_C(1) << (value % 64);
}

static void rbc_lir_set_remove(RbcLirSet *set, uint32_t value) {
    if (value) set->words[value / 64] &= ~(UINT64_C(1) << (value % 64));
}

static bool rbc_lir_set_has(const RbcLirSet *set, uint32_t value) {
    return (set->words[value / 64] & (UINT64_C(1) << (value % 64))) != 0;
}

static void rbc_lir_set_union(RbcLirSet *set, const RbcLirSet *other) {
    for (size_t w = 0; w < 4; ++w) set->words[w] |= other->words[w];
}

static void rbc_lir_live_out(RbcLirCompiler *c, RbcLirRegisters *r,
    XrLirBlock block, const XrLirBlockView *view, RbcLirSet *live) {
    XR_DCHECK(view->instruction_count, "a verified block has a terminator");
    *live = (RbcLirSet) {0};
    XrLirInstructionView end;
    xr_lir_instruction_view(c->input, block, view->instruction_count - 1, &end);
    for (uint32_t e = 0; e < end.edge_count; ++e)
        rbc_lir_set_union(live, &r->live[end.edges[e].target]);
}

static void rbc_lir_live_instruction(RbcLirSet *live, const XrLirInstructionView *i) {
    rbc_lir_set_remove(live, i->result);
    for (uint32_t a = 0; a < i->operand_count; ++a)
        rbc_lir_set_add(live, i->operands[a]);
    for (uint32_t e = 0; e < i->edge_count; ++e)
        for (uint32_t a = 0; a < i->edges[e].argument_count; ++a)
            rbc_lir_set_add(live, i->edges[e].arguments[a]);
}

static void rbc_lir_liveness(RbcLirCompiler *c, RbcLirRegisters *r) {
    bool changed;
    do {
        changed = false;
        for (uint32_t b = c->view.block_count; b > 0; --b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->input, c->function, b - 1, &block);
            xr_lir_block_view(c->input, block, &view);
            RbcLirSet live;
            rbc_lir_live_out(c, r, block, &view, &live);
            for (uint32_t n = view.instruction_count; n > 0; --n) {
                XrLirInstructionView i;
                xr_lir_instruction_view(c->input, block, n - 1, &i);
                rbc_lir_live_instruction(&live, &i);
            }
            for (uint32_t p = 0; p < view.parameter_count; ++p)
                rbc_lir_set_remove(&live, view.parameters[p]);
            if (memcmp(&live, &r->live[block.id], sizeof(live))) {
                r->live[block.id] = live;
                changed = true;
            }
        }
    } while (changed);
}

static void rbc_lir_conflict(RbcLirRegisters *r, uint32_t a, uint32_t b) {
    if (a == b) return;
    rbc_lir_set_add(&r->conflicts[a], b);
    rbc_lir_set_add(&r->conflicts[b], a);
}

static void rbc_lir_conflict_live(RbcLirCompiler *c, RbcLirRegisters *r,
    uint32_t definition, const RbcLirSet *live) {
    if (!definition) return;
    for (uint32_t v = 1; v <= c->view.value_count; ++v)
        if (rbc_lir_set_has(live, v)) rbc_lir_conflict(r, definition, v);
}

static void rbc_lir_interference(RbcLirCompiler *c, RbcLirRegisters *r) {
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        RbcLirSet live;
        rbc_lir_live_out(c, r, block, &view, &live);
        for (uint32_t n = view.instruction_count; n > 0; --n) {
            XrLirInstructionView i;
            xr_lir_instruction_view(c->input, block, n - 1, &i);
            rbc_lir_conflict_live(c, r, i.result, &live);
            // Boolean materialization writes its default before reading operands.
            if (i.op == XR_LIR_ICMP)
                for (uint32_t a = 0; a < i.operand_count; ++a)
                    rbc_lir_conflict(r, i.result, i.operands[a]);
            rbc_lir_live_instruction(&live, &i);
        }
        for (uint32_t p = 0; p < view.parameter_count; ++p) {
            rbc_lir_conflict_live(c, r, view.parameters[p], &live);
            for (uint32_t q = p + 1; q < view.parameter_count; ++q)
                rbc_lir_conflict(r, view.parameters[p], view.parameters[q]);
        }
    }
    for (uint32_t p = 0; p < c->view.parameter_count; ++p)
        for (uint32_t q = p + 1; q < c->view.parameter_count; ++q)
            rbc_lir_conflict(r, c->view.parameters[p], c->view.parameters[q]);
}

static uint32_t rbc_lir_group(RbcLirRegisters *r, uint32_t value) {
    while (r->parent[value] != value) value = r->parent[value];
    return value;
}

static void rbc_lir_join(RbcLirRegisters *r, uint32_t parameter, uint32_t actual) {
    uint32_t a = rbc_lir_group(r, parameter), b = rbc_lir_group(r, actual);
    if (a == b || (r->fixed[a] != UINT32_MAX && r->fixed[b] != UINT32_MAX)) return;
    for (size_t w = 0; w < 4; ++w)
        if (r->members[a].words[w] & r->conflicts[b].words[w]) return;
    r->parent[b] = a;
    rbc_lir_set_union(&r->members[a], &r->members[b]);
    rbc_lir_set_union(&r->conflicts[a], &r->conflicts[b]);
    if (r->fixed[b] != UINT32_MAX) r->fixed[a] = r->fixed[b];
}

static void rbc_lir_coalesce(RbcLirCompiler *c, RbcLirRegisters *r) {
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        XrLirInstructionView end;
        xr_lir_instruction_view(c->input, block, view.instruction_count - 1, &end);
        for (uint32_t e = 0; e < end.edge_count; ++e) {
            XrLirEdgeView edge = end.edges[e];
            XrLirBlockView target;
            xr_lir_block_view(c->input,
                (XrLirBlock) {c->input, c->function.id, edge.target}, &target);
            for (uint32_t p = 0; p < edge.argument_count; ++p)
                rbc_lir_join(r, target.parameters[p], edge.arguments[p]);
        }
    }
    for (size_t v = 0; v < sizeof(c->slots) / sizeof(c->slots[0]); ++v)
        c->slots[v] = UINT32_MAX;
    c->slot_count = c->view.parameter_count;
    for (uint32_t p = 0; p < c->view.parameter_count; ++p)
        c->slots[rbc_lir_group(r, c->view.parameters[p])] = p;
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        uint32_t group = rbc_lir_group(r, v);
        if (c->slots[group] == UINT32_MAX) c->slots[group] = c->slot_count++;
    }
    for (uint32_t v = 1; v <= c->view.value_count; ++v)
        c->slots[v] = c->slots[rbc_lir_group(r, v)];
}

static bool rbc_lir_assign_slots(RbcLirCompiler *c) {
    size_t blocks = c->view.block_count;
    if (blocks == SIZE_MAX || blocks + 1 >
            (SIZE_MAX - sizeof(RbcLirRegisters)) / sizeof(RbcLirSet))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "register workspace exceeds address space");
    size_t bytes = sizeof(RbcLirRegisters) + (blocks + 1) * sizeof(RbcLirSet);
    RbcLirRegisters *r = xr_malloc(bytes);
    if (!r)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate register workspace");
    memset(r, 0, bytes);
    // Fixed set members align the adjacent per-block storage as well.
    r->live = (RbcLirSet *) (r + 1);
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        r->parent[v] = v;
        r->fixed[v] = UINT32_MAX;
        rbc_lir_set_add(&r->members[v], v);
    }
    for (uint32_t p = 0; p < c->view.parameter_count; ++p)
        r->fixed[c->view.parameters[p]] = p;
    rbc_lir_liveness(c, r);
    rbc_lir_interference(c, r);
    rbc_lir_coalesce(c, r);
    xr_free(r);
    return true;
}

static bool rbc_lir_copies(RbcLirCompiler *c, XrLirEdgeView edge, bool emit,
    bool *scratch) {
    XrLirBlockView target;
    xr_lir_block_view(c->input,
        (XrLirBlock) {c->input, c->function.id, edge.target}, &target);
    uint32_t destinations[255], sources[255], count = 0;
    for (uint32_t p = 0; p < edge.argument_count; ++p) {
        uint32_t destination = c->slots[target.parameters[p]], source = c->slots[edge.arguments[p]];
        if (destination != source) {
            destinations[count] = destination;
            sources[count++] = source;
        }
    }
    uint32_t remaining = count;
    while (remaining) {
        uint32_t first = UINT32_MAX;
        bool progress = false;
        for (uint32_t p = 0; p < count; ++p) {
            if (destinations[p] == UINT32_MAX) continue;
            if (first == UINT32_MAX) first = p;
            bool used = false;
            for (uint32_t q = 0; q < count; ++q)
                if (destinations[q] != UINT32_MAX && sources[q] == destinations[p]) used = true;
            if (used) continue;
            if (emit && !rbc_lir_emit(c, XR_RBC_MOV, destinations[p], sources[p], 0, 0))
                return false;
            destinations[p] = UINT32_MAX;
            --remaining;
            progress = true;
        }
        if (progress) continue;
        XR_DCHECK(first != UINT32_MAX, "a pending parallel copy has an old source");
        uint32_t source = sources[first];
        if (emit && !rbc_lir_emit(c, XR_RBC_MOV, c->slot_count, source, 0, 0)) return false;
        *scratch = true;
        for (uint32_t p = 0; p < count; ++p)
            if (destinations[p] != UINT32_MAX && sources[p] == source) sources[p] = c->slot_count;
    }
    return true;
}
