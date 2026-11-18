/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_set_region.inc.c - Structural local Array loop preparations
 */
typedef struct LirArrayRegionUse {
    LirArrayForwardUse direct;
    uint32_t saving_uses, next_retain;
} LirArrayRegionUse;
typedef struct LirArrayRegionProof {
    const XrLirModule *module;
    const XrLirFunctionData *function;
    const XrLirOwnershipPlan *plan;
    uint32_t function_id;
    LirGraph graph;
    LirArrayRegionUse *uses;
    uint32_t *pairs, *moves, *blocks;
    uint64_t *marks;
    XrLirArraySetRegionStep *steps;
    uint32_t step_count, step_capacity, block_count;
    uint32_t retain_head, retain_block, retain_drop;
} LirArrayRegionProof;

static void lir_array_regions_free(LirArrayRegionTables *tables) {
    xr_free(tables->regions.data); xr_free(tables->steps.data);
    *tables = (LirArrayRegionTables){0};
}
static bool lir_array_region_append(LirArrayRegionVector *vector, const void *item, size_t stride) {
    if (vector->count == UINT32_MAX) return false;
    if (vector->count == vector->capacity) {
        uint32_t capacity = vector->capacity ? vector->capacity : 8;
        if (capacity <= UINT32_MAX / 2) capacity *= 2;
        else capacity = UINT32_MAX;
        if (capacity <= vector->count || capacity > SIZE_MAX / stride) return false;
        void *memory = xr_malloc((size_t)capacity * stride);
        if (!memory) return false;
        if (vector->count) memcpy(memory, vector->data, (size_t)vector->count * stride);
        xr_free(vector->data); vector->data = memory; vector->capacity = capacity;
    }
    memcpy((unsigned char *)vector->data + (size_t)vector->count * stride, item, stride);
    ++vector->count;
    return true;
}
static bool lir_array_region_scalar(XrLirType type) {
    return type == XR_LIR_I1 || type == XR_LIR_I64;
}
static bool lir_array_region_array(const XrLirFunctionData *f, uint32_t value) {
    return value && value <= f->value_count && f->values[value - 1].type == XR_LIR_ARRAY &&
        f->values[value - 1].array_element == XR_LIR_I64;
}
static bool lir_array_region_needed(const XrLirOwnershipPlan *plan, uint32_t function) {
    XrLirOwnershipFunction row = plan->functions[function - 1];
    bool set = false, create = false, read = false;
    for (uint32_t n = 0; n < row.fact_count; ++n) {
        const XrLirOwnershipFact *fact = &plan->facts[row.fact_offset + n];
        set |= fact->kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION && fact->array_element == XR_LIR_I64 &&
            fact->array_operation == XR_LIR_ARRAY_OP_SET;
        create |= fact->kind == XR_LIR_OWNERSHIP_BINDING_NEW && fact->forwarding_mode != 0;
        read |= fact->kind == XR_LIR_OWNERSHIP_BINDING_READ && fact->forwarding_mode != 0;
    }
    return set && create && read;
}
static bool lir_array_region_array_role(const XrLirModule *m, const XrLirInstructionView *op, uint32_t argument) {
    if (argument) return false;
    if (op->op == XR_LIR_ARRAY_DROP || op->op == XR_LIR_ARRAY_RETAIN ||
        (op->op == XR_LIR_CELL_NEW && op->type == XR_LIR_ARRAY_CELL)) return true;
    const XrLirArrayIntrinsic *shape = op->op == XR_LIR_CALL_INTRINSIC ?
        xr_lir_internal_array_intrinsic(m, op->intrinsic) : NULL;
    return shape && (shape->operation == XR_LIR_ARRAY_OP_LEN || shape->operation == XR_LIR_ARRAY_OP_GET);
}
static bool lir_array_region_recorded_pair(const LirArrayRegionProof *p, uint32_t block, uint32_t instruction) {
    const XrLirBlockData *body = &p->function->blocks[block];
    const XrLirInstructionView *op = &body->instructions[instruction - 1].view;
    if (!op->result || !p->pairs[op->result] || op->operand_count != 1 || instruction >= body->instruction_count)
        return false;
    bool read = op->op == XR_LIR_CELL_READ;
    uint32_t cell = read ? op->operands[0] : op->result, array = read ? op->result : op->operands[0];
    const XrLirOwnershipFact *fact = &p->plan->facts[p->pairs[op->result] - 1];
    const XrLirInstructionView *drop = &body->instructions[instruction].view;
    return fact->kind == (XrLirOwnershipFactKind)(read ? XR_LIR_OWNERSHIP_BINDING_READ : XR_LIR_OWNERSHIP_BINDING_NEW) &&
        fact->block == block + 1 && fact->instruction == instruction && fact->value == cell && fact->target_value == array &&
        fact->type == XR_LIR_ARRAY_CELL && fact->array_element == XR_LIR_I64 &&
        fact->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_NONE && fact->forwarding_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE &&
        fact->forwarding_drop == instruction + 1 && lir_array_forward_cell(p->function, cell) &&
        lir_array_region_array(p->function, array) && drop->operand_count == 1 && drop->operands[0] == op->operands[0] &&
        drop->op == (XrLirOp)(read ? XR_LIR_ARRAY_CELL_DROP : XR_LIR_ARRAY_DROP);
}
/* One whole-function use inventory bounds every candidate. It does not prove
 * uniqueness or extend the lifetime of an operand borrow. */
static bool lir_array_region_inventory(LirArrayRegionProof *p) {
    const XrLirFunctionData *f = p->function;
    XrLirOwnershipFunction row = p->plan->functions[p->function_id - 1];
    for (uint32_t n = 0; n < row.fact_count; ++n) {
        uint32_t ordinal = row.fact_offset + n;
        const XrLirOwnershipFact *fact = &p->plan->facts[ordinal];
        if (fact->kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER && fact->type == XR_LIR_ARRAY &&
            fact->array_element == XR_LIR_I64) {
            if (!fact->value || fact->value > f->value_count) return false;
            uint32_t *moves = &p->moves[(size_t)fact->value * 2];
            if (!moves[0]) moves[0] = ordinal + 1;
            else if (!moves[1]) moves[1] = ordinal + 1;
            else return false;
        }
        if (!fact->forwarding_mode) continue;
        uint32_t value = fact->kind == XR_LIR_OWNERSHIP_BINDING_NEW ? fact->value : fact->target_value;
        if (!value || value > f->value_count || p->pairs[value]) return false;
        p->pairs[value] = ordinal + 1;
    }
    for (uint32_t b = 0; b < f->block_count; ++b)
        for (uint32_t i = 0; i < f->blocks[b].instruction_count; ++i) {
            const XrLirInstructionView *op = &f->blocks[b].instructions[i].view;
            /* Each retain remains a whole-function barrier until its exact
             * local endpoint death is proven after the use inventory. */
            if (op->op == XR_LIR_ARRAY_RETAIN) {
                if (!lir_array_region_array(f, op->result) || op->operand_count != 1 ||
                    !lir_array_region_array(f, op->operands[0])) return false;
                p->uses[op->result].next_retain = p->retain_head; p->retain_head = op->result;
            }
            /* An unpaired constructor or read can preserve a second owner
             * outside the loop even when its entry Array has no alias use. */
            if (((op->op == XR_LIR_CELL_NEW && op->type == XR_LIR_ARRAY_CELL) ||
                (op->op == XR_LIR_CELL_READ && op->type == XR_LIR_ARRAY)) &&
                !lir_array_region_recorded_pair(p, b, i + 1)) return false;
            if (op->type == XR_LIR_ARRAY && op->op != XR_LIR_CELL_READ && op->op != XR_LIR_ARRAY_RETAIN) {
                const XrLirArrayIntrinsic *origin = op->op == XR_LIR_CALL_INTRINSIC ?
                    xr_lir_internal_array_intrinsic(p->module, op->intrinsic) : NULL;
                /* Ref, State and unknown producers may leave another owner
                 * alive outside this function's selected local lineage. */
                if (!origin || origin->result_element != XR_LIR_I64 ||
                    (origin->operation != XR_LIR_ARRAY_OP_NEW && origin->operation != XR_LIR_ARRAY_OP_REPEAT)) return false;
            }
            for (uint32_t a = 0; a < op->operand_count; ++a) {
                uint32_t value = op->operands[a];
                LirArrayForwardUse *use = &p->uses[value].direct;
                lir_array_forward_use(use, b + 1, i + 1);
                XrLirType type = f->values[value - 1].type;
                if (type == XR_LIR_ARRAY && !lir_array_region_array_role(p->module, op, a)) use->blocked = true;
                if (type == XR_LIR_ARRAY_CELL && !lir_array_forward_role(p->module, op, a)) use->blocked = true;
                if (type == XR_LIR_ARRAY && !a && (op->op == XR_LIR_ARRAY_RETAIN ||
                    (op->op == XR_LIR_CELL_NEW && op->type == XR_LIR_ARRAY_CELL)) && p->uses[value].saving_uses < 2)
                    ++p->uses[value].saving_uses;
            }
            for (uint32_t e = 0; e < op->edge_count; ++e)
                for (uint32_t a = 0; a < op->edges[e].argument_count; ++a)
                    lir_array_forward_use(&p->uses[op->edges[e].arguments[a]].direct, b + 1, i + 1);
        }
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        if (f->values[v - 1].type == XR_LIR_ARRAY && f->values[v - 1].parameter && !f->values[v - 1].block)
            return false;
        if (f->values[v - 1].type == XR_LIR_ARRAY_CELL &&
            (!lir_array_forward_cell(f, v) || p->uses[v].direct.blocked ||
                p->uses[v].direct.block != f->values[v - 1].block))
            return false;
    }
    return true;
}
/* This private graph has finished RPO and dominator construction. Subsequent
 * region readers use only entry, predecessors, offsets and dominance queries.
 * Reuse order/rank for tree intervals, next/cursor for child lists and stack
 * for iterative traversal. One cold linear pass makes every query constant. */
static void lir_array_region_dominance_index(LirGraph *g) {
    memset(g->next, 0, (size_t)g->count * sizeof(*g->next));
    for (uint32_t b = 0; b < g->count; ++b) if (b != g->entry) {
        uint32_t parent = g->idom[b];
        XR_DCHECK(parent < g->count, "reachable block has a dominator");
        g->cursor[b] = g->next[parent]; g->next[parent] = b + 1;
    }
    uint32_t depth = 1, ordinal = 0;
    g->stack[0] = g->entry; g->order[g->entry] = ordinal++;
    while (depth) {
        uint32_t block = g->stack[depth - 1], child = g->next[block];
        if (child) {
            --child; g->next[block] = g->cursor[child];
            g->order[child] = ordinal++; g->stack[depth++] = child;
        } else {
            g->rank[block] = ordinal; --depth;
        }
    }
    XR_DCHECK(ordinal == g->count, "dominator intervals cover every reachable block");
}
static bool lir_array_region_dominates(const LirGraph *g, uint32_t header, uint32_t block) {
    return g->order[header] <= g->order[block] && g->rank[block] <= g->rank[header];
}
static uint32_t lir_array_region_retain_death(const LirArrayRegionProof *p, uint32_t value,
    uint32_t block, uint32_t instruction, uint32_t saving_uses) {
    const LirArrayRegionUse *use = &p->uses[value];
    const XrLirBlockData *body = &p->function->blocks[block - 1];
    if (use->direct.blocked || use->direct.block != block || use->saving_uses != saving_uses ||
        use->direct.last <= instruction || use->direct.last >= body->instruction_count) return 0;
    const XrLirInstructionView *last = &body->instructions[use->direct.last - 1].view;
    return last->op == XR_LIR_ARRAY_DROP && last->operand_count == 1 && last->operands[0] == value ? use->direct.last : 0;
}
/* All retained copies must have one exact local endpoint death. Their blocks
 * must lie on one dominator chain: incomparable blocks cannot both dominate
 * any candidate entry. Reduce that chain once, never once per region. */
static bool lir_array_region_retains(LirArrayRegionProof *p) {
    for (uint32_t value = p->retain_head; value; value = p->uses[value].next_retain) {
        const XrLirValueData *definition = &p->function->values[value - 1];
        const XrLirInstructionView *retain = xr_lir_internal_value_definition(p->function, value);
        if (!retain || retain->op != XR_LIR_ARRAY_RETAIN || retain->operand_count != 1) return false;
        uint32_t block = definition->block, instruction = definition->instruction + 1;
        uint32_t death = lir_array_region_retain_death(p, retain->operands[0], block, instruction, 1);
        if (!death) death = lir_array_region_retain_death(p, value, block, instruction, 0);
        if (!death) return false;
        if (!p->retain_block || lir_array_region_dominates(&p->graph, p->retain_block - 1, block - 1)) {
            if (p->retain_block != block) p->retain_drop = 0;
            p->retain_block = block;
            if (death > p->retain_drop) p->retain_drop = death;
        } else if (!lir_array_region_dominates(&p->graph, block - 1, p->retain_block - 1)) return false;
    }
    return true;
}
/* The body may have any number of blocks, but only one straight-line route.
 * Predecessor cardinality rules out side entries and other hidden backedges. */
static bool lir_array_region_shape(LirArrayRegionProof *p, uint32_t header, uint32_t body_edge,
    XrLirArraySetRegion *region) {
    const LirGraph *g = &p->graph;
    const XrLirFunctionData *f = p->function;
    const XrLirInstructionView *term = lir_terminator(f, header);
    if (term->op != XR_LIR_CONDBR || term->edge_count != 2 ||
        g->offsets[header + 1] - g->offsets[header] != 2) return false;
    uint32_t latch = UINT32_MAX, entry = UINT32_MAX;
    for (uint32_t n = g->offsets[header]; n < g->offsets[header + 1]; ++n) {
        uint32_t predecessor = g->predecessors[n];
        if (lir_array_region_dominates(g, header, predecessor)) {
            if (latch != UINT32_MAX) return false;
            latch = predecessor;
        } else {
            if (entry != UINT32_MAX) return false;
            entry = predecessor;
        }
    }
    if (entry == UINT32_MAX || latch == UINT32_MAX) return false;
    if (p->retain_block && (p->retain_block == header + 1 ||
        !lir_array_region_dominates(g, p->retain_block - 1, header) ||
        !lir_array_region_dominates(g, p->retain_block - 1, entry) ||
        (p->retain_block == entry + 1 && p->retain_drop >= f->blocks[entry].instruction_count))) return false;
    p->block_count = 0;
    uint32_t block = term->edges[body_edge].target - 1;
    uint64_t stamp = (uint64_t)header * 2 + body_edge + 1;
    while (block != header) {
        /* This route starts at the header and every visited block has its
         * sole actual incoming edge on that route. Its dominance follows
         * without walking each block's increasingly long dominator chain. */
        if (p->marks[block] == stamp || g->offsets[block + 1] - g->offsets[block] != 1) return false;
        p->marks[block] = stamp; p->blocks[p->block_count++] = block;
        const XrLirInstructionView *next = lir_terminator(f, block);
        if (next->op != XR_LIR_BR || next->edge_count != 1) return false;
        block = next->edges[0].target - 1;
    }
    uint32_t exit = term->edges[1 - body_edge].target - 1;
    if (!p->block_count || p->blocks[p->block_count - 1] != latch || exit == header || p->marks[exit] == stamp)
        return false;
    uint32_t entry_edge = UINT32_MAX;
    const XrLirInstructionView *incoming = lir_terminator(f, entry);
    for (uint32_t e = 0; e < incoming->edge_count; ++e)
        if (incoming->edges[e].target == header + 1) {
            if (entry_edge != UINT32_MAX) return false;
            entry_edge = e;
        }
    if (entry_edge == UINT32_MAX) return false;
    *region = (XrLirArraySetRegion){.function=p->function_id,.header=header + 1,.entry_block=entry + 1,
        .entry_edge=entry_edge + 1,.latch=latch + 1,.exit_edge=2 - body_edge,.exit_block=exit + 1,
        .body_block_count=p->block_count};
    return true;
}
static bool lir_array_region_step(LirArrayRegionProof *p, XrLirArraySetRegionStep step) {
    if (p->step_count == p->step_capacity) return false;
    p->steps[p->step_count++] = step;
    return true;
}
static bool lir_array_region_move(const LirArrayRegionProof *p, uint32_t block, uint32_t edge,
    uint32_t argument, uint32_t source, uint32_t target) {
    const XrLirInstructionView *term = lir_terminator(p->function, block);
    const uint32_t *moves = &p->moves[(size_t)source * 2];
    for (uint32_t n = 0; n < 2; ++n) if (moves[n]) {
        const XrLirOwnershipFact *fact = &p->plan->facts[moves[n] - 1];
        if (fact->block == block + 1 && fact->instruction == p->function->blocks[block].instruction_count &&
            fact->edge == edge + 1 && fact->argument == argument && fact->target_block == term->edges[edge].target &&
            fact->target_value == target && fact->value == source && fact->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE &&
            !fact->hold_transfer_mode) return true;
    }
    return false;
}
static bool lir_array_region_edge(LirArrayRegionProof *p, uint32_t block, uint32_t edge,
    XrLirArraySetRegionStepKind kind, uint32_t *current) {
    const XrLirFunctionData *f = p->function;
    const XrLirInstructionView *term = lir_terminator(f, block);
    const XrLirEdgeView *selected = &term->edges[edge];
    const XrLirBlockData *target = &f->blocks[selected->target - 1];
    uint32_t source = 0, destination = 0;
    for (uint32_t a = 0; a < selected->argument_count; ++a) {
        uint32_t value = selected->arguments[a], parameter = target->parameters[a];
        if (lir_array_region_array(f, parameter)) {
            if (destination || !lir_array_region_array(f, value) || p->uses[value].direct.blocked ||
                (kind != XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE && value != *current) ||
                !lir_array_region_move(p, block, edge, a, value, parameter)) return false;
            source = value; destination = parameter;
        } else if (!lir_array_region_scalar(f->values[parameter - 1].type) ||
            !lir_array_region_scalar(f->values[value - 1].type)) return false;
    }
    if (!destination) return false;
    *current = destination;
    return lir_array_region_step(p, (XrLirArraySetRegionStep){.kind=kind,.block=block + 1,
        .instruction=f->blocks[block].instruction_count,.edge=edge + 1,.source=source,.target=destination});
}
static bool lir_array_region_pure(const XrLirFunctionData *f, const XrLirInstructionView *op) {
    if (!lir_array_region_scalar(op->type)) return false;
    switch (op->op) {
    case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
    case XR_LIR_NEG: case XR_LIR_ICMP: case XR_LIR_INTCAST: break;
    default: return false;
    }
    for (uint32_t a = 0; a < op->operand_count; ++a)
        if (!lir_array_region_scalar(f->values[op->operands[a] - 1].type)) return false;
    return true;
}
static bool lir_array_region_pair(LirArrayRegionProof *p, uint32_t block, uint32_t instruction,
    uint32_t *current, bool read) {
    const XrLirFunctionData *f = p->function;
    const XrLirBlockData *body = &f->blocks[block];
    const XrLirInstructionView *op = &body->instructions[instruction - 1].view;
    if (!lir_array_region_recorded_pair(p, block, instruction) || op->operands[0] != *current) return false;
    uint32_t cell = read ? *current : op->result, array = read ? op->result : *current;
    if (p->uses[cell].direct.blocked || p->uses[cell].direct.block != block + 1 || p->uses[array].direct.blocked ||
        f->values[array - 1].block != block + 1) return false;
    const LirArrayForwardUse *dying = &p->uses[*current].direct;
    if (dying->block != block + 1 || dying->last != instruction + 1) return false;
    uint32_t source = *current; *current = op->result;
    return lir_array_region_step(p, (XrLirArraySetRegionStep){
        .kind=read ? XR_LIR_ARRAY_SET_REGION_READ_CANDIDATE : XR_LIR_ARRAY_SET_REGION_NEW_CANDIDATE,
        .block=block + 1,.instruction=instruction,.source=source,.target=*current,.drop=instruction + 1});
}
static bool lir_array_region_operation(LirArrayRegionProof *p, const XrLirInstructionView *op,
    uint32_t current, bool header) {
    const XrLirArrayIntrinsic *shape = op->op == XR_LIR_CALL_INTRINSIC ?
        xr_lir_internal_array_intrinsic(p->module, op->intrinsic) : NULL;
    if (!shape || !shape->parameter_count || shape->parameter_elements[0] != XR_LIR_I64 ||
        !op->operand_count || op->operands[0] != current || p->uses[current].direct.blocked) return false;
    bool array = lir_array_region_array(p->function, current);
    if (shape->operation == XR_LIR_ARRAY_OP_LEN) return array && op->operand_count == 1;
    if (!header && shape->operation == XR_LIR_ARRAY_OP_GET)
        return array && op->operand_count == 2 && p->function->values[op->operands[1] - 1].type == XR_LIR_I64;
    return !header && shape->operation == XR_LIR_ARRAY_OP_SET && !array &&
        p->function->values[current - 1].type == XR_LIR_ARRAY_CELL && op->operand_count == 3 &&
        p->function->values[op->operands[1] - 1].type == XR_LIR_I64 &&
        p->function->values[op->operands[2] - 1].type == XR_LIR_I64;
}
static bool lir_array_region_body(LirArrayRegionProof *p, uint32_t block, uint32_t *current,
    XrLirArraySetRegion *region, bool header) {
    const XrLirBlockData *body = &p->function->blocks[block];
    for (uint32_t i = 0; i + 1 < body->instruction_count; ++i) {
        const XrLirInstructionView *op = &body->instructions[i].view;
        if (lir_array_region_pure(p->function, op)) continue;
        if (!header && (op->op == XR_LIR_CELL_NEW || op->op == XR_LIR_CELL_READ)) {
            if (!lir_array_region_pair(p, block, i + 1, current, op->op == XR_LIR_CELL_READ)) return false;
            ++i; continue;
        }
        if (!lir_array_region_operation(p, op, *current, header)) return false;
        const XrLirArrayIntrinsic *shape = xr_lir_internal_array_intrinsic(p->module, op->intrinsic);
        if (shape->operation == XR_LIR_ARRAY_OP_SET) {
            if (region->set_block) return false;
            region->set_block = block + 1; region->set_instruction = i + 1;
        }
    }
    return true;
}
static bool lir_array_region_prove(LirArrayRegionProof *p, uint32_t header, uint32_t body_edge,
    XrLirArraySetRegion *region) {
    if (!lir_array_region_shape(p, header, body_edge, region)) return false;
    p->step_count = 0; uint32_t current = 0;
    if (!lir_array_region_edge(p, region->entry_block - 1, region->entry_edge - 1,
        XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE, &current)) return false;
    region->entry_value = p->steps[0].source; region->canonical_value = current;
    if (!lir_array_region_body(p, header, &current, region, true) ||
        !lir_array_region_edge(p, header, body_edge, XR_LIR_ARRAY_SET_REGION_EDGE_MOVE, &current)) return false;
    for (uint32_t n = 0; n < p->block_count; ++n)
        if (!lir_array_region_body(p, p->blocks[n], &current, region, false) ||
            !lir_array_region_edge(p, p->blocks[n], 0, XR_LIR_ARRAY_SET_REGION_EDGE_MOVE, &current)) return false;
    if (current != region->canonical_value || !region->set_block) return false;
    current = region->canonical_value;
    if (!lir_array_region_edge(p, header, region->exit_edge - 1, XR_LIR_ARRAY_SET_REGION_EXIT_MOVE, &current)) return false;
    region->exit_value = current; region->step_count = p->step_count;
    return true;
}
static void lir_array_region_proof_free(LirArrayRegionProof *p) {
    xr_free(p->graph.memory); xr_free(p->uses); xr_free(p->pairs); xr_free(p->moves);
    xr_free(p->blocks); xr_free(p->marks); xr_free(p->steps);
}
static XrLirStatus lir_array_regions_function(LirArrayRegionProof *p, LirArrayRegionTables *tables) {
    const XrLirFunctionData *f = p->function;
    if (f->kind == XR_LIR_FUNCTION_EXTERNAL) return XR_LIR_OK;
    if (!lir_array_region_needed(p->plan, p->function_id)) return XR_LIR_OK;
    XrLirStatus status = lir_graph_create(f, &p->graph);
    if (status != XR_LIR_OK) return status;
    if (!lir_graph_order(f, &p->graph)) return XR_LIR_VERIFY_ERROR;
    lir_graph_dominators(&p->graph);
    lir_array_region_dominance_index(&p->graph);
    uint64_t values = (uint64_t)f->value_count + 1, steps = values + f->block_count + 2;
    if (steps > UINT32_MAX) return XR_LIR_OUT_OF_MEMORY;
    p->uses = lir_plan_allocate((size_t)values, sizeof(*p->uses));
    p->pairs = lir_plan_allocate((size_t)values, sizeof(*p->pairs));
    p->moves = lir_plan_allocate((size_t)values, 2 * sizeof(*p->moves));
    p->blocks = lir_plan_allocate(f->block_count, sizeof(*p->blocks));
    p->marks = lir_plan_allocate(f->block_count, sizeof(*p->marks));
    p->steps = lir_plan_allocate((size_t)steps, sizeof(*p->steps)); p->step_capacity = (uint32_t)steps;
    if (!p->uses || !p->pairs || !p->moves || !p->blocks || !p->marks || !p->steps) return XR_LIR_OUT_OF_MEMORY;
    if (!lir_array_region_inventory(p) || !lir_array_region_retains(p)) return XR_LIR_OK;
    for (uint32_t b = 0; b < f->block_count; ++b)
        for (uint32_t e = 0; e < 2; ++e) {
            XrLirArraySetRegion region;
            if (!lir_array_region_prove(p, b, e, &region)) continue;
            region.first_step = tables->steps.count;
            for (uint32_t n = 0; n < p->step_count; ++n)
                if (!lir_array_region_append(&tables->steps, &p->steps[n], sizeof(p->steps[n]))) return XR_LIR_OUT_OF_MEMORY;
            if (!lir_array_region_append(&tables->regions, &region, sizeof(region))) return XR_LIR_OUT_OF_MEMORY;
        }
    return XR_LIR_OK;
}
static XrLirStatus lir_plan_array_set_regions(XrLirOwnershipPlan *plan,
    const XrLirModule *module, const XrLirFunctionData *f, uint32_t function) {
    XrLirOwnershipFunction *row = &plan->functions[function - 1];
    row->region_offset = plan->array_regions.regions.count;
    LirArrayRegionProof proof = {.module=module,.function=f,.plan=plan,.function_id=function};
    XrLirStatus status = lir_array_regions_function(&proof, &plan->array_regions);
    row->region_count = plan->array_regions.regions.count - row->region_offset;
    lir_array_region_proof_free(&proof);
    return status;
}
static bool lir_array_region_equal(XrLirArraySetRegion a, XrLirArraySetRegion b) {
    return a.function == b.function && a.header == b.header && a.entry_block == b.entry_block &&
        a.entry_edge == b.entry_edge && a.latch == b.latch && a.exit_edge == b.exit_edge && a.exit_block == b.exit_block &&
        a.entry_value == b.entry_value && a.canonical_value == b.canonical_value && a.exit_value == b.exit_value &&
        a.body_block_count == b.body_block_count && a.set_block == b.set_block && a.set_instruction == b.set_instruction &&
        a.first_step == b.first_step && a.step_count == b.step_count;
}
static bool lir_array_region_step_equal(XrLirArraySetRegionStep a, XrLirArraySetRegionStep b) {
    return a.kind == b.kind && a.block == b.block && a.instruction == b.instruction && a.edge == b.edge &&
        a.source == b.source && a.target == b.target && a.drop == b.drop;
}
static bool lir_array_region_proof_fact(const XrLirOwnershipFact *fact) {
    return fact->kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION || fact->forwarding_mode != 0 ||
        (fact->kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER && fact->type == XR_LIR_ARRAY) ||
        ((fact->kind == XR_LIR_OWNERSHIP_BINDING_NEW || fact->kind == XR_LIR_OWNERSHIP_BINDING_READ) &&
            fact->type == XR_LIR_ARRAY_CELL);
}
static bool lir_array_region_fact_equal(XrLirOwnershipFact a, XrLirOwnershipFact b) {
    return a.kind == b.kind && a.function == b.function && a.block == b.block && a.instruction == b.instruction &&
        a.value == b.value && a.argument == b.argument && a.edge == b.edge && a.target_block == b.target_block &&
        a.target_value == b.target_value && a.type == b.type && a.array_element == b.array_element &&
        a.array_operation == b.array_operation && a.transfer_mode == b.transfer_mode &&
        a.hold_transfer_mode == b.hold_transfer_mode && a.forwarding_mode == b.forwarding_mode &&
        a.forwarding_drop == b.forwarding_drop && a.array_result == b.array_result &&
        a.array_index == b.array_index && a.array_item == b.array_item;
}
static bool lir_array_region_inputs_equal(const XrLirOwnershipPlan *a, const XrLirOwnershipPlan *b, uint32_t function) {
    XrLirOwnershipFunction x = a->functions[function], y = b->functions[function];
    if (x.fact_offset > a->fact_count || x.fact_count > a->fact_count - x.fact_offset ||
        y.fact_offset > b->fact_count || y.fact_count > b->fact_count - y.fact_offset ||
        x.region_offset != y.region_offset || x.region_count != y.region_count) return false;
    uint32_t i = 0, j = 0;
    while (true) {
        while (i < x.fact_count && !lir_array_region_proof_fact(&a->facts[x.fact_offset + i])) ++i;
        while (j < y.fact_count && !lir_array_region_proof_fact(&b->facts[y.fact_offset + j])) ++j;
        if (i == x.fact_count || j == y.fact_count) return i == x.fact_count && j == y.fact_count;
        if (!lir_array_region_fact_equal(a->facts[x.fact_offset + i], b->facts[y.fact_offset + j])) return false;
        ++i; ++j;
    }
}
static bool lir_array_region_plans_equal(const XrLirOwnershipPlan *a, const XrLirOwnershipPlan *b) {
    if (a->function_count != b->function_count || a->array_regions.regions.count != b->array_regions.regions.count ||
        a->array_regions.steps.count != b->array_regions.steps.count) return false;
    for (uint32_t f = 0; f < a->function_count; ++f)
        if (!lir_array_region_inputs_equal(a, b, f)) return false;
    const XrLirArraySetRegion *x = a->array_regions.regions.data, *y = b->array_regions.regions.data;
    for (uint32_t n = 0; n < a->array_regions.regions.count; ++n)
        if (!lir_array_region_equal(x[n], y[n])) return false;
    const XrLirArraySetRegionStep *u = a->array_regions.steps.data, *v = b->array_regions.steps.data;
    for (uint32_t n = 0; n < a->array_regions.steps.count; ++n)
        if (!lir_array_region_step_equal(u[n], v[n])) return false;
    return true;
}
XR_FUNC XrLirStatus xr_lir_ownership_array_regions_verify(const XrLirModule *module,
    const XrLirOwnershipPlan *plan) {
    if (!plan || !xr_lir_module_is_readable(module) || module->function_count != plan->function_count)
        return XR_LIR_INVALID_ARGUMENT;
    /* Only the explicit inspection boundary pays for an independent complete
     * producer. In particular, an old empty directory cannot suppress proof
     * through its old eligibility, forwarding or selected-edge inputs. */
    XrLirOwnershipPlan *fresh = NULL;
    XrLirStatus status = xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){module,plan->types,plan->type_count},
        &fresh, NULL);
    if (status != XR_LIR_OK) return status;
    bool same = lir_array_region_plans_equal(plan, fresh);
    xr_lir_ownership_plan_destroy(fresh);
    return same ? XR_LIR_OK : XR_LIR_VERIFY_ERROR;
}
XR_FUNC uint32_t xr_lir_ownership_array_region_count(const XrLirOwnershipPlan *plan) {
    return plan ? plan->array_regions.regions.count : 0;
}
XR_FUNC uint32_t xr_lir_ownership_array_region_step_count(const XrLirOwnershipPlan *plan) {
    return plan ? plan->array_regions.steps.count : 0;
}
XR_FUNC XrLirStatus xr_lir_ownership_array_region_at(const XrLirOwnershipPlan *plan,
    uint32_t ordinal, XrLirArraySetRegion *out) {
    if (!plan || !out || ordinal >= plan->array_regions.regions.count) return XR_LIR_INVALID_ARGUMENT;
    *out = ((const XrLirArraySetRegion *)plan->array_regions.regions.data)[ordinal];
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_ownership_array_region_step_at(const XrLirOwnershipPlan *plan,
    uint32_t ordinal, XrLirArraySetRegionStep *out) {
    if (!plan || !out || ordinal >= plan->array_regions.steps.count) return XR_LIR_INVALID_ARGUMENT;
    *out = ((const XrLirArraySetRegionStep *)plan->array_regions.steps.data)[ordinal];
    return XR_LIR_OK;
}
