/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_string_windows.inc.c - Closed String loops over real owning slots
 */
typedef struct LirStringWindowCarry {
    uint32_t text, index, suffix;
    size_t edge, empty_slots[7], count;
} LirStringWindowCarry;

static bool lir_string_window_follow(const XrLirValueHomes *homes, const XrLirFunctionData *function,
    uint32_t block, const XrLirEdgeView *edge, LirStringWindowCarry *carry, bool back) {
    size_t first = 0, end = homes->edge_count;
    while (first < end) {
        size_t at = first + (end - first) / 2;
        if (homes->edges[at].block < block) first = at + 1;
        else end = at;
    }
    if (first == homes->edge_count || !edge->argument_count || edge->argument_count > 3) return false;
    const XrLirHomeEdge *move = &homes->edges[first];
    if (move->block != block || move->edge != 1 || !move->direct_header_move || move->target_block != edge->target)
        return false;
    XrLirHomeTransfer transport = homes->transfers[move->first];
    if (transport.source != carry->text) return false;
    const XrLirBlockData *target = &function->blocks[edge->target - 1];
    if (target->parameter_count != edge->argument_count) return false;
    bool has_suffix = false, has_index = false;
    for (uint32_t n = 0; n < edge->argument_count; ++n) {
        if (target->parameters[n] == transport.target) continue;
        if (homes->values[edge->arguments[n]].stationary_string == carry->suffix &&
            xr_lir_home_stationary_edge(homes, edge->arguments[n], target->parameters[n])) {
            if (has_suffix) return false;
            has_suffix = true;
            continue;
        }
        if (edge->arguments[n] != carry->index || has_index) return false;
        carry->index = target->parameters[n];
        has_index = true;
    }
    if (has_suffix != (carry->suffix != 0) || (back && !has_index)) return false;
    carry->edge = first;
    carry->text = transport.target;
    const XrLirValueHome *home = &homes->values[carry->text];
    carry->empty_slots[carry->count++] = home->temporary_first_owner;
    if (!back) carry->empty_slots[carry->count++] = home->first_owner;
    return true;
}

/* The skipped path consists only of a bound test, owner/index forwarding,
 * an append, and an increment. A non-stationary suffix must already have a
 * private literal cache; a cold cache makes the runtime guard decline. */
static bool lir_home_string_window(const LirHomeBuilder *b, const XrLirFunctionData *function,
    uint32_t header_id, XrLirStringWindow *out, size_t *latch_edge, uint32_t *body_id, uint32_t *bridge_id) {
    const XrLirBlockData *header = &function->blocks[header_id - 1];
    if (header->parameter_count < 2 || header->parameter_count > 3 || header->instruction_count != 2)
        return false;
    uint32_t index = 0, owner = 0, stable = 0;
    const XrLirValueHomes *homes = b->homes;
    for (uint32_t p = 0; p < header->parameter_count; ++p) {
        uint32_t id = header->parameters[p];
        XrLirType type = function->values[id - 1].type;
        if (type == XR_LIR_I64 && !index) index = id;
        else if (type == XR_LIR_STRING) {
            uint32_t producer = homes->values[id].stationary_string;
            if (producer && !stable) stable = producer;
            else if (!producer && !owner) owner = id;
            else return false;
        } else return false;
    }
    if (!index || !owner) return false;
    const XrLirInstructionView *cmp = &header->instructions[0].view, *branch = &header->instructions[1].view;
    if (cmp->op != XR_LIR_ICMP || cmp->predicate != XR_LIR_LT || cmp->operands[0] != index ||
        cmp->operands[1] == index || function->values[cmp->operands[1] - 1].type != XR_LIR_I64 ||
        branch->op != XR_LIR_CONDBR || branch->operand_count != 1 || branch->operands[0] != cmp->result) return false;
    LirStringWindowCarry carry = {.text=owner,.index=index,.suffix=stable};
    const XrLirEdgeView *edge = &branch->edges[0];
    if (!lir_string_window_follow(homes, function, header_id, edge, &carry, false)) return false;
    *body_id = edge->target;
    const XrLirBlockData *body = &function->blocks[*body_id - 1];
    if (body->instruction_count == 1) {
        const XrLirInstructionView *bridge = &body->instructions[0].view;
        if (bridge->op != XR_LIR_BR || !lir_string_window_follow(homes, function, *body_id, &bridge->edges[0],
            &carry, false)) return false;
        *bridge_id = *body_id;
        *body_id = bridge->edges[0].target;
        body = &function->blocks[*body_id - 1];
    }
    if (body->instruction_count != (stable ? 4u : 6u) || body->parameter_count != header->parameter_count) return false;
    const XrLirInstructionView *literal = NULL, *append = NULL, *step = NULL, *add = NULL, *drop = NULL;
    for (uint32_t n = 0; n + 1 < body->instruction_count; ++n) {
        const XrLirInstructionView *op = &body->instructions[n].view;
        if (op->op == XR_LIR_STRING_LITERAL) literal = op;
        else if (op->op == XR_LIR_CONST) step = op;
        else if (op->op == XR_LIR_ADD) add = op;
        else if (op->op == XR_LIR_STRING_DROP) drop = op;
        else if (xr_lir_string_role(b->compiler.input, op) == XR_LIR_STRING_OP_APPEND_OWNED) append = op;
        else return false;
    }
    if (!append || !add || !step || append->operands[0] != carry.text ||
        step->type != XR_LIR_I64 || step->bits != 1 || add->type != XR_LIR_I64 ||
        add->operands[0] != carry.index || add->operands[1] != step->result) return false;
    const XrLirValueHome *suffix = &homes->values[append->operands[1]], *result = &homes->values[append->result];
    size_t suffix_slot;
    if (stable) {
        if (suffix->stationary_string != stable) return false;
        suffix_slot = suffix->first_owner;
    } else {
        if (!literal || !drop || append->operands[1] != literal->result || drop->operands[0] != literal->result ||
            !suffix->literal_append_borrow) return false;
        suffix_slot = suffix->literal_cache_owner;
    }
    if (suffix_slot == SIZE_MAX || result->temporary_first_owner == SIZE_MAX) return false;
    carry.empty_slots[carry.count++] = result->first_owner;
    carry.empty_slots[carry.count++] = result->temporary_first_owner;
    carry.text = append->result;
    carry.index = add->result;
    const XrLirInstructionView *latch = &body->instructions[body->instruction_count - 1].view;
    if (latch->op != XR_LIR_BR || latch->edges[0].target != header_id ||
        !lir_string_window_follow(homes, function, *body_id, &latch->edges[0], &carry, true) ||
        carry.text != owner || carry.index != index) return false;
    *out = (XrLirStringWindow){.header=header_id,.index=index,.next_index=add->result,.limit=cmp->operands[1],
        .header_owner=homes->values[owner].first_owner,.latch_owner=result->first_owner,.suffix_owner=suffix_slot,
        .empty_count=carry.count};
    memcpy(out->empty_slots, carry.empty_slots, carry.count * sizeof(*out->empty_slots));
    *latch_edge = carry.edge;
    return true;
}

static uint8_t *lir_string_window_predecessors(const XrLirFunctionData *function) {
    uint8_t *counts = xr_calloc(function->block_count, sizeof(*counts));
    if (!counts) return NULL;
    for (uint32_t n = 0; n < function->block_count; ++n) {
        const XrLirBlockData *block = &function->blocks[n];
        const XrLirInstructionView *term = &block->instructions[block->instruction_count - 1].view;
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            uint8_t *count = &counts[term->edges[e].target - 1];
            if (*count < 2) ++*count;
        }
    }
    return counts;
}

static bool lir_home_string_windows(LirHomeBuilder *b) {
    if (!b->homes->owned_frame_needed || !b->edges.count) return true;
    const XrLirFunctionData *function = &b->compiler.input->functions[b->compiler.function.id - 1];
    LirHomeVector windows = {.stride=sizeof(XrLirStringWindow)};
    uint8_t *predecessors = NULL;
    for (uint32_t header = 1; header <= function->block_count; ++header) {
        XrLirStringWindow window;
        size_t latch = 0;
        uint32_t body = 0, bridge = 0;
        if (!lir_home_string_window(b, function, header, &window, &latch, &body, &bridge) ||
            body == function->entry || bridge == function->entry) continue;
        if (!predecessors) predecessors = lir_string_window_predecessors(function);
        if (!predecessors) {
            lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OOM, "cannot count String window predecessors");
            break;
        }
        if (predecessors[body - 1] != 1 || (bridge && predecessors[bridge - 1] != 1)) continue;
        if (!lir_home_append(b, &windows, &window)) break;
        /* The VM consumes the plan at a selected latch. An incoming formal
         * has a stable register there without any extra SSA/PC remapping. */
        const XrLirValueData *limit = &function->values[window.limit - 1];
        if (!limit->block && limit->parameter)
            ((XrLirHomeEdge *)b->edges.data)[latch].string_window = (uint32_t)windows.count;
    }
    xr_free(predecessors);
    b->homes->string_windows = windows.data;
    b->homes->string_window_count = windows.count;
    return b->compiler.status == XR_LIR_VALUE_LAYOUT_OK;
}

XR_FUNCDEF const XrLirStringWindow *xr_lir_value_homes_string_window(const XrLirValueHomes *homes, uint32_t header) {
    if (!homes) return NULL;
    size_t first = 0, end = homes->string_window_count;
    while (first < end) {
        size_t at = first + (end - first) / 2;
        if (homes->string_windows[at].header < header) first = at + 1;
        else end = at;
    }
    return first < homes->string_window_count && homes->string_windows[first].header == header ?
        &homes->string_windows[first] : NULL;
}
