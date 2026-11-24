/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_affine_repeat.inc.c - Prove repeated pointwise I64 updates
 */

typedef struct LirArrayAffine {
    uint64_t scale, bias;
    bool known;
} LirArrayAffine;

typedef struct LirArrayLoopCarry {
    uint32_t owner, round;
} LirArrayLoopCarry;

static LirArrayAffine lir_array_affine_operand(const XrLirFunctionData *f, uint32_t block,
    uint32_t value, const LirArrayAffine *expression) {
    uint64_t bits;
    if (lir_array_element_constant(f, value, &bits)) return (LirArrayAffine){0, bits, true};
    const XrLirValueData *def = &f->values[value - 1];
    return def->type == XR_LIR_I64 && def->block == block && !def->parameter ?
        expression[def->instruction] : (LirArrayAffine){0};
}

static LirArrayAffine lir_array_affine_evaluate(const XrLirFunctionData *f, uint32_t block,
    const XrLirInstructionView *op, const LirArrayAffine *expression) {
    if (op->type != XR_LIR_I64) return (LirArrayAffine){0};
    if (op->op == XR_LIR_CONST) return (LirArrayAffine){0, op->bits, true};
    if (op->op != XR_LIR_ADD && op->op != XR_LIR_SUB && op->op != XR_LIR_NEG && op->op != XR_LIR_MUL)
        return (LirArrayAffine){0};
    LirArrayAffine a = lir_array_affine_operand(f, block, op->operands[0], expression);
    if (!a.known) return a;
    if (op->op == XR_LIR_NEG) return (LirArrayAffine){0 - a.scale, 0 - a.bias, true};
    LirArrayAffine b = lir_array_affine_operand(f, block, op->operands[1], expression);
    if (!b.known) return b;
    if (op->op == XR_LIR_ADD) return (LirArrayAffine){a.scale + b.scale, a.bias + b.bias, true};
    if (op->op == XR_LIR_SUB) return (LirArrayAffine){a.scale - b.scale, a.bias - b.bias, true};
    if (a.scale && b.scale) return (LirArrayAffine){0};
    return (LirArrayAffine){a.scale * b.bias + b.scale * a.bias, a.bias * b.bias, true};
}

static LirArrayAffine lir_array_affine_element(const LirArrayRegionProof *p,
    const XrLirArraySetRegion *region) {
    /* A bounded single-block value walk avoids allocation or recursive DAG traversal. */
    LirArrayAffine expression[32] = {{0}};
    if (!p->get_count || region->primary_instruction > sizeof(expression) / sizeof(expression[0]))
        return (LirArrayAffine){0};
    const XrLirBlockData *body = &p->function->blocks[region->primary_block - 1];
    uint32_t reads = 0;
    for (uint32_t i = 0; i + 1 < region->primary_instruction; ++i) {
        const XrLirInstructionView *op = &body->instructions[i].view;
        const XrLirArrayIntrinsic *shape = op->op == XR_LIR_CALL_INTRINSIC ?
            xr_lir_internal_array_intrinsic(p->module, op->intrinsic) : NULL;
        if (shape && shape->operation == XR_LIR_ARRAY_OP_GET) {
            /* The shared region proof already established the same I64 owner
             * and IV. Every admitted GET precedes the sole SET. */
            expression[i] = (LirArrayAffine){1, 0, true};
            ++reads;
        } else {
            expression[i] = lir_array_affine_evaluate(p->function, region->primary_block, op, expression);
        }
    }
    if (reads != p->get_count) return (LirArrayAffine){0};
    const XrLirInstructionView *set = &body->instructions[region->primary_instruction - 1].view;
    return lir_array_affine_operand(p->function, region->primary_block, set->operands[2], expression);
}

static bool lir_array_affine_edge(const LirArrayRegionProof *p, uint32_t block,
    uint32_t edge, uint32_t inner_iv, LirArrayLoopCarry *carry) {
    const XrLirEdgeView *selected = &lir_terminator(p->function, block)->edges[edge];
    const XrLirBlockData *target = &p->function->blocks[selected->target - 1];
    if (inner_iv ? target->parameter_count != 3 : !target->parameter_count || target->parameter_count > 2)
        return false;
    LirArrayLoopCarry next = {0};
    for (uint32_t a = 0; a < selected->argument_count; ++a) {
        uint32_t value = selected->arguments[a], parameter = target->parameters[a];
        uint64_t bits;
        if (inner_iv && parameter == inner_iv) {
            if (!lir_array_element_constant(p->function, value, &bits) || bits) return false;
        } else if (value == carry->owner && !next.owner && lir_array_region_array(p->function, parameter) &&
            lir_array_region_move(p, block, edge, a, value, parameter)) {
            next.owner = parameter;
        } else if (value == carry->round && !next.round && p->function->values[parameter - 1].type == XR_LIR_I64) {
            next.round = parameter;
        } else return false;
    }
    if (!next.owner || (inner_iv && !next.round)) return false;
    /* Ownership normalization may route through an owner-only bridge.
     * That block still sees the same dominating scalar definition. */
    if (!next.round) next.round = carry->round;
    *carry = next;
    return true;
}

static bool lir_array_affine_unit_step(const XrLirFunctionData *f, const XrLirInstructionView *op, uint32_t iv) {
    uint64_t one;
    if (op->op != XR_LIR_ADD || op->type != XR_LIR_I64) return false;
    uint32_t a = op->operands[0], b = op->operands[1];
    return (a == iv && lir_array_element_constant(f, b, &one) && one == 1) ||
           (b == iv && lir_array_element_constant(f, a, &one) && one == 1);
}

/* Follow the only inner exit until the enclosing header. The sole nonconstant
 * computation may be the enclosing induction increment. All intermediate
 * owner moves and scalar edge arguments are the actual verified transitions. */
static uint32_t lir_array_affine_latch(const LirArrayRegionProof *p, const XrLirArraySetRegion *region,
    LirArrayLoopCarry *carry, uint32_t *latch) {
    const XrLirFunctionData *f = p->function;
    uint32_t from = region->header - 1, edge = region->exit_edge - 1;
    bool advanced = false;
    for (uint32_t n = 0; n < f->block_count; ++n) {
        uint32_t block = lir_terminator(f, from)->edges[edge].target - 1;
        if (!lir_array_affine_edge(p, from, edge, 0, carry)) return 0;
        const XrLirInstructionView *term = lir_terminator(f, block);
        uint32_t predecessors = p->graph.offsets[block + 1] - p->graph.offsets[block];
        if (term->op == XR_LIR_CONDBR) {
            const XrLirValueData *round = &f->values[carry->round - 1];
            if (!advanced || predecessors != 2 || !round->parameter || round->block != block + 1 ||
                !lir_array_region_dominates(&p->graph, block, region->header - 1)) return 0;
            *latch = from;
            return block + 1;
        }
        if (term->op != XR_LIR_BR || predecessors != 1) return 0;
        const XrLirBlockData *body = &f->blocks[block];
        for (uint32_t i = 0; i + 1 < body->instruction_count; ++i) {
            const XrLirInstructionView *op = &body->instructions[i].view;
            if (op->op == XR_LIR_CONST) continue;
            if (advanced || !lir_array_affine_unit_step(f, op, carry->round)) return 0;
            carry->round = op->result;
            advanced = true;
        }
        from = block;
        edge = 0;
    }
    return 0;
}

static bool lir_array_affine_zero_entry(const LirArrayRegionProof *p, uint32_t header, uint32_t latch, uint32_t iv) {
    const XrLirFunctionData *f = p->function;
    const XrLirValueData *parameter = &f->values[iv - 1];
    for (uint32_t n = p->graph.offsets[header]; n < p->graph.offsets[header + 1]; ++n) {
        uint32_t block = p->graph.predecessors[n];
        if (block == latch) continue;
        if (lir_array_region_dominates(&p->graph, header, block)) return false;
        const XrLirInstructionView *term = lir_terminator(f, block);
        for (uint32_t e = 0; e < term->edge_count; ++e) if (term->edges[e].target == header + 1) {
            uint64_t start;
            return lir_array_element_constant(f, term->edges[e].arguments[parameter->instruction], &start) && !start;
        }
    }
    return false;
}

static uint32_t lir_array_affine_outer_limit(const LirArrayRegionProof *p, uint32_t outer, uint32_t round) {
    const XrLirFunctionData *f = p->function;
    const XrLirBlockData *header = &f->blocks[outer];
    const XrLirInstructionView *term = lir_terminator(f, outer);
    const XrLirInstructionView *check = xr_lir_internal_value_definition(f, term->operands[0]);
    if (!check || check->op != XR_LIR_ICMP || check->predicate != XR_LIR_LT ||
        check->operands[0] != round || f->values[check->result - 1].block != outer + 1) return 0;
    for (uint32_t i = 0; i + 1 < header->instruction_count; ++i) {
        const XrLirInstructionView *op = &header->instructions[i].view;
        if (op != check && op->op != XR_LIR_CONST) return 0;
    }
    uint32_t limit = check->operands[1];
    const XrLirValueData *value = &f->values[limit - 1];
    uint64_t bits;
    return value->type == XR_LIR_I64 && ((value->parameter && !value->block) ||
        lir_array_element_constant(f, limit, &bits)) ? limit : 0;
}

static bool lir_array_affine_enter(const LirArrayRegionProof *p, const XrLirArraySetRegion *region,
    uint32_t outer, LirArrayLoopCarry carry, uint32_t round) {
    const XrLirFunctionData *f = p->function;
    uint32_t from = outer;
    for (uint32_t n = 0; n < f->block_count; ++n) {
        uint32_t target = lir_terminator(f, from)->edges[0].target;
        if (target == region->header) {
            return from + 1 == region->entry_block && region->entry_edge == 1 &&
                lir_array_affine_edge(p, from, 0, region->range.iv, &carry) &&
                carry.round == round && carry.owner == region->canonical_value;
        }
        uint32_t block = target - 1;
        if (lir_terminator(f, block)->op != XR_LIR_BR ||
            p->graph.offsets[block + 1] - p->graph.offsets[block] != 1 ||
            !lir_array_affine_edge(p, from, 0, 0, &carry)) return false;
        const XrLirBlockData *body = &f->blocks[block];
        for (uint32_t i = 0; i + 1 < body->instruction_count; ++i)
            if (body->instructions[i].view.op != XR_LIR_CONST) return false;
        from = block;
    }
    return false;
}

static XrLirArrayRepeatedAffine lir_array_affine_repeat(LirArrayRegionProof *p,
    const XrLirArraySetRegion *region) {
    XrLirArrayRepeatedAffine none = {0};
    if (region->primary_kind != XR_LIR_ARRAY_REGION_PRIMARY_SET || region->exit_edge != 2 ||
        region->range.kind != XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN ||
        region->owner_home.kind != XR_LIR_ARRAY_SET_OWNER_HOME_CANONICAL_WINDOW) return none;
    const XrLirBlockData *inner = &p->function->blocks[region->header - 1];
    if (inner->parameter_count != 3) return none;
    uint32_t round = 0;
    for (uint32_t a = 0; a < inner->parameter_count; ++a)
        if (inner->parameters[a] != region->range.iv && inner->parameters[a] != region->canonical_value)
            round = inner->parameters[a];
    if (!round || p->function->values[round - 1].type != XR_LIR_I64 ||
        (!region->element_read && !lir_array_region_scalar_passthrough(p, region, region->range.iv, 0))) return none;
    LirArrayAffine affine = lir_array_affine_element(p, region);
    if (!affine.known) return none;
    LirArrayLoopCarry carry = {region->canonical_value, round};
    uint32_t latch = 0;
    uint32_t outer = lir_array_affine_latch(p, region, &carry, &latch);
    if (!outer) return none;
    uint32_t limit = lir_array_affine_outer_limit(p, outer - 1, carry.round);
    if (!limit || !lir_array_affine_zero_entry(p, outer - 1, latch, carry.round) ||
        !lir_array_affine_enter(p, region, outer - 1, carry, round)) return none;
    return (XrLirArrayRepeatedAffine){.round = round, .limit = limit, .scale = affine.scale, .bias = affine.bias};
}
