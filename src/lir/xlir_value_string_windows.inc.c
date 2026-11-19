/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_string_windows.inc.c - Closed String loops over real owning slots
 */
typedef struct LirStringWindowCarry {
    uint32_t header, text, index, suffix;
    size_t edge;
    LirHomeVector *slots;
} LirStringWindowCarry;

static bool lir_string_window_follow(LirHomeBuilder *b, const XrLirFunctionData *function,
    uint32_t block, uint32_t selected, LirStringWindowCarry *carry, bool require_index) {
    const XrLirValueHomes *homes = b->homes;
    const XrLirBlockData *source = &function->blocks[block - 1];
    const XrLirInstructionView *term = &source->instructions[source->instruction_count - 1].view;
    const XrLirEdgeView *edge = &term->edges[selected];
    uint32_t ordinal = selected + 1;
    size_t first = 0, end = homes->edge_count;
    while (first < end) {
        size_t at = first + (end - first) / 2;
        const XrLirHomeEdge *candidate = &homes->edges[at];
        if (candidate->block < block || (candidate->block == block && candidate->edge < ordinal)) first = at + 1;
        else end = at;
    }
    if (first == homes->edge_count || !edge->argument_count || edge->argument_count > 3) return false;
    const XrLirHomeEdge *move = &homes->edges[first];
    if (move->block != block || move->edge != ordinal || !move->direct_header_move || move->target_block != edge->target)
        return false;
    XrLirHomeTransfer transport = homes->transfers[move->first];
    if (transport.source != carry->text) return false;
    const XrLirBlockData *target = &function->blocks[edge->target - 1];
    if (target->parameter_count != edge->argument_count) return false;
    bool has_suffix = false, has_index = false;
    for (uint32_t n = 0; n < edge->argument_count; ++n) {
        if (target->parameters[n] == transport.target) continue;
        if (homes->values[edge->arguments[n]].stationary_owner == carry->suffix &&
            xr_lir_home_stationary_edge(homes, edge->arguments[n], target->parameters[n])) {
            if (has_suffix) return false;
            has_suffix = true;
            continue;
        }
        if (edge->arguments[n] != carry->index || has_index) return false;
        carry->index = target->parameters[n];
        has_index = true;
    }
    if (has_suffix != (carry->suffix != 0) || (require_index && !has_index)) return false;
    carry->edge = first;
    carry->text = transport.target;
    const XrLirValueHome *home = &homes->values[carry->text];
    return lir_home_append(b, carry->slots, &home->temporary_first_owner) &&
        (edge->target == carry->header || lir_home_append(b, carry->slots, &home->first_owner));
}

static uint8_t *lir_string_window_predecessors(const XrLirFunctionData *function) {
    uint8_t *counts = xr_calloc(function->block_count, sizeof(*counts));
    uint32_t *queue = xr_calloc(function->block_count, sizeof(*queue));
    if (!counts || !queue) {
        xr_free(counts);
        xr_free(queue);
        return NULL;
    }
    uint32_t length = 1;
    queue[0] = function->entry;
    counts[function->entry - 1] = 1;
    for (uint32_t n = 0; n < length; ++n) {
        const XrLirBlockData *block = &function->blocks[queue[n] - 1];
        const XrLirInstructionView *term = &block->instructions[block->instruction_count - 1].view;
        uint32_t selected = xr_lir_internal_known_edge(function, term);
        uint32_t first = selected == UINT32_MAX ? 0 : selected;
        uint32_t end = selected == UINT32_MAX ? term->edge_count : selected + 1;
        for (uint32_t e = first; e < end; ++e) {
            uint32_t target = term->edges[e].target;
            uint8_t *count = &counts[target - 1];
            if (*count == 0) queue[length++] = target;
            if (*count < 2) ++*count;
        }
    }
    xr_free(queue);
    return counts;
}

static bool lir_string_window_forward(LirHomeBuilder *b, const XrLirFunctionData *function,
    uint32_t *block, LirStringWindowCarry *carry, uint32_t selected) {
    if (!lir_string_window_follow(b, function, *block, selected, carry, false)) return false;
    *block = b->homes->edges[carry->edge].target_block;
    return true;
}

static bool lir_string_window_header(const XrLirFunctionData *function, const XrLirBlockData *header,
    uint32_t comparison) {
    for (uint32_t n = 0; n + 1 < header->instruction_count; ++n) {
        const XrLirInstructionView *op = &header->instructions[n].view;
        if (n == comparison || (op->op == XR_LIR_CONST && (op->type == XR_LIR_I64 || op->type == XR_LIR_I1)))
            continue;
        /* Generated C enters the window at the signed comparison. Earlier
         * definitions must be constant so none can keep a stale index. */
        if (n < comparison || op->op != XR_LIR_ICMP || op->operand_count != 2 || op->type != XR_LIR_I1 ||
            function->values[op->operands[0] - 1].type != XR_LIR_I1 ||
            function->values[op->operands[1] - 1].type != XR_LIR_I1) return false;
    }
    return true;
}

/* The skipped path consists only of a bound test, owner/index forwarding,
 * an append, and an increment. A non-stationary suffix must already have a
 * private literal cache; a cold cache makes the runtime guard decline. */
static bool lir_home_string_window(LirHomeBuilder *b, const XrLirFunctionData *function, uint32_t header_id,
    XrLirStringWindow *out, size_t *latch_edge, LirHomeVector *slots, uint8_t **predecessors) {
    const XrLirBlockData *header = &function->blocks[header_id - 1];
    if (header->parameter_count < 2 || header->parameter_count > 3 || header->instruction_count < 2)
        return false;
    uint32_t index = 0, owner = 0, stable = 0;
    const XrLirValueHomes *homes = b->homes;
    for (uint32_t p = 0; p < header->parameter_count; ++p) {
        uint32_t id = header->parameters[p];
        XrLirType type = function->values[id - 1].type;
        if (type == XR_LIR_I64 && !index) index = id;
        else if (type == XR_LIR_STRING) {
            uint32_t producer = homes->values[id].stationary_owner;
            if (producer && !stable) stable = producer;
            else if (!producer && !owner) owner = id;
            else return false;
        } else return false;
    }
    if (!index || !owner) return false;
    const XrLirInstructionView *branch = &header->instructions[header->instruction_count - 1].view;
    uint32_t order[2] = {0}, selected = 0;
    const XrLirInstructionView *cmp = xr_lir_internal_branch_i64_lt(function, branch, selected, order);
    if (!cmp || order[0] != index) {
        selected = 1;
        cmp = xr_lir_internal_branch_i64_lt(function, branch, selected, order);
    }
    if (!cmp || function->values[cmp->result - 1].block != header_id || order[0] != index || order[1] == index)
        return false;
    uint32_t comparison = function->values[cmp->result - 1].instruction;
    if (!lir_string_window_header(function, header, comparison)) return false;
    uint32_t bound = order[1];
    const XrLirInstructionView *limit = xr_lir_internal_value_definition(function, bound);
    bool constant_limit = limit && limit->op == XR_LIR_CONST;
    size_t first_empty = slots->count;
    LirStringWindowCarry carry = {.header=header_id,.text=owner,.index=index,.suffix=stable,.slots=slots};
    if (!lir_string_window_follow(b, function, header_id, selected, &carry, false)) return false;
    if (!*predecessors) *predecessors = lir_string_window_predecessors(function);
    if (!*predecessors)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OOM, "cannot count String window predecessors");
    if (!(*predecessors)[header_id - 1]) return false;
    uint32_t body_id = branch->edges[selected].target;
    const XrLirBlockData *body;
    /* A cycle in the forwarding path would either reach the header or enter
     * a block with two incoming edges. The same check excludes side entries. */
    for (;;) {
        if (body_id == header_id || body_id == function->entry || (*predecessors)[body_id - 1] != 1)
            return false;
        body = &function->blocks[body_id - 1];
        uint32_t forward = xr_lir_internal_forward_edge(function, body);
        if (forward == UINT32_MAX) break;
        if (!lir_string_window_forward(b, function, &body_id, &carry, forward)) return false;
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
        if (suffix->stationary_owner != stable) return false;
        suffix_slot = suffix->first_owner;
    } else {
        if (!literal || !drop || append->operands[1] != literal->result || drop->operands[0] != literal->result ||
            !suffix->literal_append_borrow) return false;
        suffix_slot = suffix->literal_cache_owner;
    }
    if (suffix_slot == SIZE_MAX || result->temporary_first_owner == SIZE_MAX) return false;
    if (!lir_home_append(b, slots, &result->first_owner) || !lir_home_append(b, slots, &result->temporary_first_owner))
        return false;
    carry.text = append->result;
    carry.index = add->result;
    const XrLirInstructionView *latch = &body->instructions[body->instruction_count - 1].view;
    uint32_t latch_edge_index = xr_lir_internal_known_edge(function, latch);
    if (latch_edge_index == UINT32_MAX ||
        !lir_string_window_follow(b, function, body_id, latch_edge_index, &carry, true)) return false;
    size_t actual_latch = carry.edge;
    uint32_t latch_index = carry.index;
    size_t latch_target_owner = homes->values[carry.text].first_owner;
    uint32_t next = homes->edges[actual_latch].target_block;
    /* Only the first returning block may also be entered from outside. A
     * second entry before the comparison could change the values we skip. */
    bool first = true;
    while (next != header_id) {
        if (next == function->entry || (!first && (*predecessors)[next - 1] != 1)) return false;
        const XrLirBlockData *forward = &function->blocks[next - 1];
        uint32_t edge = xr_lir_internal_forward_edge(function, forward);
        if (edge == UINT32_MAX || !lir_string_window_forward(b, function, &next, &carry, edge)) return false;
        first = false;
    }
    if (carry.text != owner || carry.index != index) return false;
    size_t latch_empty = slots->count;
    size_t header_owner = homes->values[owner].first_owner;
    for (size_t n = first_empty; n < latch_empty; ++n) {
        size_t slot = ((const size_t *)slots->data)[n];
        if (slot == result->first_owner) slot = header_owner;
        if (!lir_home_append(b, slots, &slot)) return false;
    }
    *out = (XrLirStringWindow){.header=header_id,.index=index,.next_index=add->result,.latch_index=latch_index,.limit=bound,
        .comparison=comparison,.constant_limit=constant_limit,.limit_bits=constant_limit ? limit->bits : 0,
        .header_owner=header_owner,.latch_owner=result->first_owner,.latch_target_owner=latch_target_owner,.suffix_owner=suffix_slot,
        .header_empty_first=first_empty,.latch_empty_first=latch_empty,.empty_count=latch_empty - first_empty};
    *latch_edge = actual_latch;
    return true;
}

static bool lir_home_string_windows(LirHomeBuilder *b) {
    if (!b->homes->owned_frame_needed || !b->edges.count) return true;
    const XrLirFunctionData *function = &b->compiler.input->functions[b->compiler.function.id - 1];
    LirHomeVector windows = {.stride=sizeof(XrLirStringWindow)};
    LirHomeVector slots = {.stride=sizeof(size_t)};
    uint8_t *predecessors = NULL;
    for (uint32_t header = 1; header <= function->block_count; ++header) {
        XrLirStringWindow window;
        size_t latch = 0;
        size_t first = slots.count;
        if (!lir_home_string_window(b, function, header, &window, &latch, &slots, &predecessors)) {
            slots.count = first;
            if (b->compiler.status != XR_LIR_VALUE_LAYOUT_OK) break;
            continue;
        }
        if (!lir_home_append(b, &windows, &window)) break;
        /* A nonconstant bound is live into the original comparison. The
         * closed header does not redefine it, and no skipped step changes it. */
        ((XrLirHomeEdge *)b->edges.data)[latch].string_window = (uint32_t)windows.count;
    }
    xr_free(predecessors);
    if (!slots.count) {
        xr_free(slots.data);
        slots.data = NULL;
    }
    b->homes->string_windows = windows.data;
    b->homes->string_window_count = windows.count;
    b->homes->string_window_slots = slots.data;
    b->homes->string_window_slot_count = slots.count;
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
