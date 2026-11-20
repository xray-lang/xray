/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_set_region.inc.c - Structural local Array loop preparations
 */
typedef struct LirArrayRegionUse {
    uint64_t range_stamp;
    uint32_t lane, edge_lane;
} LirArrayRegionUse;
typedef struct LirArrayRegionProof {
    const XrLirModule *module;
    const XrLirFunctionData *function;
    const XrLirOwnershipPlan *plan;
    uint32_t function_id;
    LirGraph graph;
    LirLinearLoop loop;
    LirArrayRegionUse *uses;
    uint32_t *pairs, *moves, *blocks;
    uint64_t *marks;
    XrLirArraySetRegionStep *steps;
    uint32_t step_count, step_capacity;
    uint32_t get_count, get_block, get_instruction;
    uint32_t array_accesses;
    bool push_current;
    uint64_t range_stamp;
    XrLirArraySetRegionRange range;
} LirArrayRegionProof;

static void lir_array_regions_free(LirArrayRegionTables *tables) {
    xr_free(tables->regions.data); xr_free(tables->steps.data);
    *tables = (LirArrayRegionTables){0};
}
static bool lir_plan_array_append(LirArrayRegionVector *vector, const void *item, size_t stride) {
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
    bool set = false, push = false, create = false, read = false, get = false, edge = false;
    for (uint32_t n = 0; n < row.fact_count; ++n) {
        const XrLirOwnershipFact *fact = &plan->facts[row.fact_offset + n];
        set |= fact->kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION && fact->array_element == XR_LIR_I64 &&
            fact->array_operation == XR_LIR_ARRAY_OP_SET;
        push |= fact->kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION && fact->array_element == XR_LIR_I64 &&
            fact->array_operation == XR_LIR_ARRAY_OP_PUSH;
        create |= fact->kind == XR_LIR_OWNERSHIP_BINDING_NEW && fact->type == XR_LIR_ARRAY_CELL && fact->forwarding_mode != 0;
        read |= fact->kind == XR_LIR_OWNERSHIP_BINDING_READ && fact->type == XR_LIR_ARRAY_CELL && fact->forwarding_mode != 0;
        get |= fact->kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION && fact->array_element == XR_LIR_I64 &&
            fact->array_operation == XR_LIR_ARRAY_OP_GET;
        edge |= fact->kind == XR_LIR_OWNERSHIP_SELECTED_EDGE_TRANSFER && fact->type == XR_LIR_ARRAY &&
            fact->array_element == XR_LIR_I64 && fact->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_MOVE;
    }
    return (set && create && read) || (get && edge) || (push && create && read && edge);
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
        fact->forwarding_drop == instruction + 1 && lir_forward_cell(p->function, cell, XR_LIR_ARRAY_CELL) &&
        lir_array_region_array(p->function, array) && drop->operand_count == 1 && drop->operands[0] == op->operands[0] &&
        drop->op == (XrLirOp)(read ? XR_LIR_ARRAY_CELL_DROP : XR_LIR_ARRAY_DROP);
}
static bool lir_array_region_index_transfers(LirArrayRegionProof *p) {
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
        if (fact->type != XR_LIR_ARRAY_CELL || !fact->forwarding_mode) continue;
        uint32_t value = fact->kind == XR_LIR_OWNERSHIP_BINDING_NEW ? fact->value : fact->target_value;
        if (!value || value > f->value_count || p->pairs[value]) return false;
        p->pairs[value] = ordinal + 1;
    }
    return true;
}
static bool lir_array_region_shape(LirArrayRegionProof *p, uint32_t header, uint32_t body_edge,
    XrLirArraySetRegion *region) {
    LirLinearLoop *loop = &p->loop;
    if (!lir_loop_linear_edges(p->function, &p->graph, header, body_edge, loop) ||
        !lir_loop_linear_body(p->function, &p->graph, p->blocks, p->marks, loop)) return false;
    *region = (XrLirArraySetRegion){.function=p->function_id,.header=header + 1,.entry_block=loop->entry + 1,
        .entry_edge=loop->entry_edge + 1,.latch=loop->latch + 1,.exit_edge=2 - body_edge,.exit_block=loop->exit + 1,
        .body_block_count=loop->body_count};
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
enum { LIR_ARRAY_RANGE_CURRENT = 1 };
static uint32_t lir_array_region_lane(const LirArrayRegionProof *p, uint32_t value) {
    return p->uses[value].range_stamp == p->range_stamp ? p->uses[value].lane : 0;
}
static void lir_array_region_range_begin(LirArrayRegionProof *p, const XrLirArraySetRegion *region,
    uint32_t body_edge) {
    const XrLirFunctionData *f = p->function;
    const XrLirInstructionView *term = lir_terminator(f, region->header - 1);
    uint32_t order[2];
    const XrLirInstructionView *compare = xr_lir_internal_branch_i64_lt(f, term, body_edge, order);
    if (!compare || f->values[compare->result - 1].block != region->header) return;
    uint32_t condition = compare->result, iv = order[0], length = order[1];
    if (iv == length) return;
    const XrLirValueData *definition = &f->values[iv - 1];
    const XrLirInstructionView *bound = xr_lir_internal_value_definition(f, length);
    const XrLirArrayIntrinsic *shape = bound && bound->op == XR_LIR_CALL_INTRINSIC ?
        xr_lir_internal_array_intrinsic(p->module, bound->intrinsic) : NULL;
    if (!definition->parameter || definition->block != region->header) return;
    const XrLirValueData *end = &f->values[length - 1];
    bool same = shape && shape->operation == XR_LIR_ARRAY_OP_LEN && bound->type == XR_LIR_I64 &&
        bound->operand_count == 1 && bound->operands[0] == region->canonical_value && end->block == region->header;
    // A dominating SSA definition has settled before this loop, including
    // parameters of blocks reached through different pre-loop paths.
    bool independent = end->type == XR_LIR_I64 &&
        ((!end->block && end->parameter) ||
         (end->block && lir_loop_dominates(&p->graph, end->block - 1, region->entry_block - 1)) ||
         (bound && bound->op == XR_LIR_CONST));
    // A header parameter also qualifies if the latch forwards it unchanged.
    bool carried = end->type == XR_LIR_I64 && end->parameter && end->block == region->header;
    if (!same && !independent && !carried) return;
    const XrLirBlockData *header = &f->blocks[region->header - 1];
    uint32_t argument = 0;
    while (argument < header->parameter_count && header->parameters[argument] != iv) ++argument;
    if (argument == header->parameter_count) return;
    LirLoopUnitStep step;
    if (!lir_loop_unit_step(f, &p->graph, &p->loop, p->marks, iv, &step)) return;
    const XrLirValueData *update = &f->values[step.update - 1];
    const XrLirInstructionView *entry = lir_terminator(f, region->entry_block - 1);
    p->range = (XrLirArraySetRegionRange){.kind=same ? XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN :
            XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT,
        .iv=iv,.iv_argument=argument,.entry_iv=entry->edges[region->entry_edge - 1].arguments[argument],
        .length=length,.length_instruction=bound ? end->instruction + 1 : 0,
        .condition=condition,.condition_instruction=f->values[condition - 1].instruction + 1,
        .increment=step.update,.increment_block=update->block,.increment_instruction=update->instruction + 1,
        .one=step.one,.latch_iv=step.backedge_argument};
    p->uses[iv].range_stamp = p->range_stamp; p->uses[iv].lane = LIR_ARRAY_RANGE_CURRENT;
}
/* A selected edge assigns parameters simultaneously. Scratch belongs to the
 * unique target parameters, so even a latch that reads header parameters must
 * finish every source read before publishing any destination lane. */
static void lir_array_region_range_edge(LirArrayRegionProof *p, const XrLirEdgeView *edge) {
    if (!p->range.kind) return;
    const XrLirBlockData *target = &p->function->blocks[edge->target - 1];
    XR_DCHECK(edge->argument_count == target->parameter_count, "selected edge has exact parameter arity");
    for (uint32_t a = 0; a < edge->argument_count; ++a)
        p->uses[target->parameters[a]].edge_lane = lir_array_region_lane(p, edge->arguments[a]);
    for (uint32_t a = 0; a < edge->argument_count; ++a) {
        LirArrayRegionUse *use = &p->uses[target->parameters[a]];
        XR_DCHECK(use->edge_lane <= LIR_ARRAY_RANGE_CURRENT, "range lane came from the old edge environment");
        use->range_stamp = p->range_stamp; use->lane = use->edge_lane;
    }
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
            if (destination || !lir_array_region_array(f, value) ||
                (kind != XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE && value != *current) ||
                !lir_array_region_move(p, block, edge, a, value, parameter)) return false;
            source = value; destination = parameter;
        } else if (!lir_array_region_scalar(f->values[parameter - 1].type) ||
            !lir_array_region_scalar(f->values[value - 1].type)) return false;
    }
    if (!destination) return false;
    lir_array_region_range_edge(p, selected);
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
        !op->operand_count || op->operands[0] != current) return false;
    bool array = lir_array_region_array(p->function, current);
    if (shape->operation == XR_LIR_ARRAY_OP_LEN) return array && op->operand_count == 1;
    if (!header && shape->operation == XR_LIR_ARRAY_OP_GET)
        return array && op->operand_count == 2 && p->function->values[op->operands[1] - 1].type == XR_LIR_I64;
    return !header && !array && p->function->values[current - 1].type == XR_LIR_ARRAY_CELL &&
        ((shape->operation == XR_LIR_ARRAY_OP_SET && op->operand_count == 3 &&
          p->function->values[op->operands[1] - 1].type == XR_LIR_I64 &&
          p->function->values[op->operands[2] - 1].type == XR_LIR_I64) ||
         (shape->operation == XR_LIR_ARRAY_OP_PUSH && op->operand_count == 2 &&
          p->function->values[op->operands[1] - 1].type == XR_LIR_I64));
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
        if (p->array_accesses < 2) ++p->array_accesses;
        if (p->range.kind && (shape->operation == XR_LIR_ARRAY_OP_GET || shape->operation == XR_LIR_ARRAY_OP_SET) &&
            lir_array_region_lane(p, op->operands[1]) != LIR_ARRAY_RANGE_CURRENT)
            p->range = (XrLirArraySetRegionRange){0};
        if (shape->operation == XR_LIR_ARRAY_OP_SET || shape->operation == XR_LIR_ARRAY_OP_PUSH) {
            if (region->primary_kind) return false;
            region->primary_kind = shape->operation == XR_LIR_ARRAY_OP_SET ?
                XR_LIR_ARRAY_REGION_PRIMARY_SET : XR_LIR_ARRAY_REGION_PRIMARY_PUSH;
            region->primary_block = block + 1; region->primary_instruction = i + 1;
            if (shape->operation == XR_LIR_ARRAY_OP_PUSH)
                p->push_current = lir_array_region_lane(p, op->operands[1]) == LIR_ARRAY_RANGE_CURRENT;
        } else if (shape->operation == XR_LIR_ARRAY_OP_GET) {
            if (p->get_count < 2) ++p->get_count;
            p->get_block = block + 1; p->get_instruction = i + 1;
        }
    }
    return true;
}
/* Compare each observed phi with its own source on the actual backedge.
 * Resolving sources independently preserves parallel edge assignment. */
static bool lir_array_region_scalar_passthrough(const LirArrayRegionProof *p,
    uint32_t advancing, uint32_t only) {
    const XrLirFunctionData *f = p->function;
    const XrLirBlockData *header = &f->blocks[p->loop.header];
    const XrLirEdgeView *backedge = &lir_terminator(f, p->loop.latch)->edges[0];
    for (uint32_t a = 0; a < header->parameter_count; ++a) {
        uint32_t parameter = header->parameters[a];
        if (!lir_array_region_scalar(f->values[parameter - 1].type)) continue;
        if (only ? parameter != only : parameter == advancing) continue;
        uint32_t origin = lir_loop_body_origin(f, &p->graph, &p->loop, p->marks, backedge->arguments[a]);
        if (origin != parameter) return false;
    }
    return true;
}

static bool lir_array_region_preserves_bound(const LirArrayRegionProof *p) {
    const XrLirValueData *limit = &p->function->values[p->range.length - 1];
    return !limit->parameter || limit->block != p->loop.header + 1 ||
        lir_array_region_scalar_passthrough(p, 0, p->range.length);
}

static XrLirArrayReadSum lir_array_region_read_sum(const LirArrayRegionProof *p,
    const XrLirArraySetRegion *region) {
    XrLirArrayReadSum sum = {0};
    if (region->primary_kind != XR_LIR_ARRAY_REGION_PRIMARY_GET) return sum;
    /* A reduction does not perform ownership or binding work. Every original
     * body operation is already proved pure, except the one checked read. */
    for (uint32_t n = 1; n + 1 < p->step_count; ++n)
        if (p->steps[n].kind != XR_LIR_ARRAY_SET_REGION_EDGE_MOVE) return (XrLirArrayReadSum){0};
    const XrLirFunctionData *f = p->function;
    const XrLirBlockData *header = &f->blocks[p->loop.header];
    const XrLirBlockData *body = &f->blocks[region->primary_block - 1];
    uint32_t item = body->instructions[region->primary_instruction - 1].view.result;
    const XrLirEdgeView *latch = &lir_terminator(f, p->loop.latch)->edges[0];
    for (uint32_t a = 0; a < header->parameter_count; ++a) {
        uint32_t parameter = header->parameters[a];
        if (parameter == region->range.iv || parameter == region->canonical_value) continue;
        uint32_t incoming = latch->arguments[a];
        uint32_t origin = lir_loop_body_origin(f, &p->graph, &p->loop, p->marks, incoming);
        if (origin == parameter) continue;
        const XrLirInstructionView *add = xr_lir_internal_value_definition(f, origin);
        if (sum.accumulator || !add || add->op != XR_LIR_ADD || add->type != XR_LIR_I64 ||
            add->operand_count != 2 || f->values[parameter - 1].type != XR_LIR_I64)
            return (XrLirArrayReadSum){0};
        uint32_t left = lir_loop_body_origin(f, &p->graph, &p->loop, p->marks, add->operands[0]);
        uint32_t right = lir_loop_body_origin(f, &p->graph, &p->loop, p->marks, add->operands[1]);
        if (!((left == item && right == parameter) || (left == parameter && right == item)))
            return (XrLirArrayReadSum){0};
        sum.accumulator = parameter;
        sum.next_accumulator = incoming;
    }
    if (sum.accumulator && region->range.kind == XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT) {
        const XrLirInstructionView *limit = xr_lir_internal_value_definition(f, region->range.length);
        if (limit && limit->op == XR_LIR_CONST) sum.limit_bits = limit->bits;
        else sum.limit = region->range.length;
    }
    return sum;
}

static bool lir_array_region_prove(LirArrayRegionProof *p, const XrLirArraySetRegion *shape, XrLirArraySetRegion *region) {
    const LirLinearLoop *loop = &p->loop;
    uint32_t header = loop->header, body_edge = loop->body_edge;
    *region = *shape;
    /* Distinct routes cannot read each other's loop-carried lanes. */
    ++p->range_stamp;
    p->step_count = p->get_count = p->array_accesses = 0;
    p->push_current = false;
    p->range = (XrLirArraySetRegionRange){0}; uint32_t current = 0;
    if (!lir_array_region_edge(p, region->entry_block - 1, region->entry_edge - 1,
        XR_LIR_ARRAY_SET_REGION_ENTRY_MOVE, &current)) return false;
    region->entry_value = p->steps[0].source; region->canonical_value = current;
    lir_array_region_range_begin(p, region, body_edge);
    if (!lir_array_region_body(p, header, &current, region, true) ||
        !lir_array_region_edge(p, header, body_edge, XR_LIR_ARRAY_SET_REGION_EDGE_MOVE, &current)) return false;
    for (uint32_t n = 0; n < p->loop.body_count; ++n)
        if (!lir_array_region_body(p, p->blocks[n], &current, region, false) ||
            !lir_array_region_edge(p, p->blocks[n], 0, XR_LIR_ARRAY_SET_REGION_EDGE_MOVE, &current)) return false;
    if (current != region->canonical_value) return false;
    if (p->range.latch_iv && p->range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN) region->range = p->range;
    if (!region->primary_kind) {
        if (p->get_count != 1 || !p->range.latch_iv) return false;
        if (p->range.kind == XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT &&
            !lir_array_region_preserves_bound(p)) return false;
        /* A read route never performs a Cell round trip or a release. */
        for (uint32_t n = 1; n < p->step_count; ++n)
            if (p->steps[n].kind != XR_LIR_ARRAY_SET_REGION_EDGE_MOVE) return false;
        region->range = p->range;
        region->primary_kind = XR_LIR_ARRAY_REGION_PRIMARY_GET;
        region->primary_block = p->get_block; region->primary_instruction = p->get_instruction;
    } else if (region->primary_kind == XR_LIR_ARRAY_REGION_PRIMARY_PUSH) {
        if (p->array_accesses != 1 || !p->range.latch_iv ||
            p->range.kind != XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT ||
            !lir_array_region_preserves_bound(p)) return false;
        region->range = p->range;
        /* Counted append can skip the scalar body only when the original item
         * is the IV and every other carried scalar stays unchanged. */
        region->push_counted = p->push_current &&
            lir_array_region_scalar_passthrough(p, p->range.iv, 0);
    }
    /* SET without a proved entire range uses the same owner and edges.
     * Its receivers must retain each original element bounds check. */
    p->range = (XrLirArraySetRegionRange){0};
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
    lir_loop_dominance_index(&p->graph);
    uint64_t values = (uint64_t)f->value_count + 1, steps = values + f->block_count + 2;
    if (steps > UINT32_MAX) return XR_LIR_OUT_OF_MEMORY;
    p->uses = lir_plan_allocate((size_t)values, sizeof(*p->uses));
    p->pairs = lir_plan_allocate((size_t)values, sizeof(*p->pairs));
    p->moves = lir_plan_allocate((size_t)values, 2 * sizeof(*p->moves));
    p->blocks = lir_plan_allocate(f->block_count, sizeof(*p->blocks));
    p->marks = lir_plan_allocate(f->block_count, sizeof(*p->marks));
    p->steps = lir_plan_allocate((size_t)steps, sizeof(*p->steps)); p->step_capacity = (uint32_t)steps;
    if (!p->uses || !p->pairs || !p->moves || !p->blocks || !p->marks || !p->steps) return XR_LIR_OUT_OF_MEMORY;
    if (!lir_array_region_index_transfers(p)) return XR_LIR_OK;
    for (uint32_t b = 0; b < f->block_count; ++b)
        for (uint32_t e = 0; e < 2; ++e) {
            XrLirArraySetRegion shape, region;
            if (!lir_array_region_shape(p, b, e, &shape)) continue;
            if (!lir_array_region_prove(p, &shape, &region)) continue;
            region.read_sum = lir_array_region_read_sum(p, &region);
            region.first_step = tables->steps.count;
            for (uint32_t n = 0; n < p->step_count; ++n)
                if (!lir_plan_array_append(&tables->steps, &p->steps[n], sizeof(p->steps[n]))) return XR_LIR_OUT_OF_MEMORY;
            if (!lir_plan_array_append(&tables->regions, &region, sizeof(region))) return XR_LIR_OUT_OF_MEMORY;
        }
    return XR_LIR_OK;
}

static XrLirStatus lir_plan_array_set_regions(XrLirOwnershipPlan *plan,
    const XrLirModule *module, const XrLirFunctionData *f, uint32_t function) {
    XrLirOwnershipFunction *row = &plan->functions[function - 1];
    row->fixed_update_offset = plan->fixed_array_updates.regions.count;
    LirArrayRegionProof proof = {.module=module,.function=f,.plan=plan,.function_id=function};
    XrLirStatus status = lir_array_regions_function(&proof, &plan->fixed_array_updates);
    row->fixed_update_count = plan->fixed_array_updates.regions.count - row->fixed_update_offset;
    lir_array_region_proof_free(&proof);
    return status;
}
static bool lir_array_region_equal(XrLirArraySetRegion a, XrLirArraySetRegion b) {
    return a.function == b.function && a.header == b.header && a.entry_block == b.entry_block &&
        a.entry_edge == b.entry_edge && a.latch == b.latch && a.exit_edge == b.exit_edge && a.exit_block == b.exit_block &&
        a.entry_value == b.entry_value && a.canonical_value == b.canonical_value && a.exit_value == b.exit_value &&
        a.body_block_count == b.body_block_count && a.primary_kind == b.primary_kind &&
        a.primary_block == b.primary_block && a.primary_instruction == b.primary_instruction &&
        a.first_step == b.first_step && a.step_count == b.step_count &&
        a.range.kind == b.range.kind && a.range.iv == b.range.iv && a.range.iv_argument == b.range.iv_argument &&
        a.range.entry_iv == b.range.entry_iv && a.range.length == b.range.length &&
        a.range.length_instruction == b.range.length_instruction && a.range.condition == b.range.condition &&
        a.range.condition_instruction == b.range.condition_instruction && a.range.increment == b.range.increment &&
        a.range.increment_block == b.range.increment_block && a.range.increment_instruction == b.range.increment_instruction &&
        a.range.one == b.range.one && a.range.latch_iv == b.range.latch_iv &&
        a.read_sum.accumulator == b.read_sum.accumulator && a.read_sum.next_accumulator == b.read_sum.next_accumulator &&
        a.read_sum.limit == b.read_sum.limit && a.read_sum.limit_bits == b.read_sum.limit_bits &&
        a.push_counted == b.push_counted;
}
static bool lir_array_region_step_equal(XrLirArraySetRegionStep a, XrLirArraySetRegionStep b) {
    return a.kind == b.kind && a.block == b.block && a.instruction == b.instruction && a.edge == b.edge &&
        a.source == b.source && a.target == b.target && a.drop == b.drop;
}
static bool lir_array_region_proof_fact(const XrLirOwnershipFact *fact) {
    return fact->kind == XR_LIR_OWNERSHIP_ARRAY_OPERATION ||
        (fact->type == XR_LIR_ARRAY_CELL && fact->forwarding_mode != 0) ||
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
        x.fixed_update_offset != y.fixed_update_offset || x.fixed_update_count != y.fixed_update_count) return false;
    uint32_t i = 0, j = 0;
    while (true) {
        while (i < x.fact_count && !lir_array_region_proof_fact(&a->facts[x.fact_offset + i])) ++i;
        while (j < y.fact_count && !lir_array_region_proof_fact(&b->facts[y.fact_offset + j])) ++j;
        if (i == x.fact_count || j == y.fact_count) return i == x.fact_count && j == y.fact_count;
        if (!lir_array_region_fact_equal(a->facts[x.fact_offset + i], b->facts[y.fact_offset + j])) return false;
        ++i; ++j;
    }
}
static bool lir_array_region_tables_equal(const LirArrayRegionTables *a, const LirArrayRegionTables *b) {
    if (a->regions.count!=b->regions.count || a->steps.count!=b->steps.count) return false;
    const XrLirArraySetRegion *x = a->regions.data, *y = b->regions.data;
    for (uint32_t n = 0; n < a->regions.count; ++n)
        if (!lir_array_region_equal(x[n], y[n])) return false;
    const XrLirArraySetRegionStep *u = a->steps.data, *v = b->steps.data;
    for (uint32_t n = 0; n < a->steps.count; ++n)
        if (!lir_array_region_step_equal(u[n], v[n])) return false;
    return true;
}
static bool lir_array_region_plans_equal(const XrLirOwnershipPlan *a, const XrLirOwnershipPlan *b) {
    if (a->function_count != b->function_count) return false;
    for (uint32_t f = 0; f < a->function_count; ++f)
        if (!lir_array_region_inputs_equal(a,b,f)) return false;
    return lir_array_region_tables_equal(&a->fixed_array_updates, &b->fixed_array_updates);
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
XR_FUNC XrLirStatus xr_lir_ownership_fixed_array_update_at(const XrLirOwnershipPlan *plan,
    uint32_t ordinal, XrLirArraySetRegion *out) {
    if (!plan || !out || ordinal >= plan->fixed_array_updates.regions.count) return XR_LIR_INVALID_ARGUMENT;
    *out = ((const XrLirArraySetRegion *)plan->fixed_array_updates.regions.data)[ordinal];
    return XR_LIR_OK;
}
XR_FUNC XrLirStatus xr_lir_ownership_fixed_array_update_step_at(const XrLirOwnershipPlan *plan,
    uint32_t ordinal, XrLirArraySetRegionStep *out) {
    if (!plan || !out || ordinal >= plan->fixed_array_updates.steps.count) return XR_LIR_INVALID_ARGUMENT;
    *out = ((const XrLirArraySetRegionStep *)plan->fixed_array_updates.steps.data)[ordinal];
    return XR_LIR_OK;
}
