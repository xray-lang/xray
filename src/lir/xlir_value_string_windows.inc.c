/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_value_string_windows.inc.c - Closed String loops over real owning slots
 */
typedef struct LirStringWindowCarry {
    uint32_t header, text, suffix;
    size_t edge;
    bool replay;
    LirHomeVector *slots, *route;
} LirStringWindowCarry;

static const XrLirEdgeView *lir_string_window_edge(const XrLirFunctionData *f, uint32_t block, uint32_t edge) {
    const XrLirBlockData *source = &f->blocks[block - 1];
    return &source->instructions[source->instruction_count - 1].view.edges[edge - 1];
}

static bool lir_string_window_scalar(XrLirType type) {
    return type == XR_LIR_I64 || type == XR_LIR_I1;
}

static bool lir_string_window_follow(LirHomeBuilder *b, const XrLirFunctionData *function,
    uint32_t *block, uint32_t selected, LirStringWindowCarry *carry) {
    const XrLirValueHomes *homes = b->homes;
    uint32_t ordinal = selected + 1;
    const XrLirEdgeView *edge = lir_string_window_edge(function, *block, ordinal);
    size_t first = 0, end = homes->edge_count;
    while (first < end) {
        size_t at = first + (end - first) / 2;
        const XrLirHomeEdge *candidate = &homes->edges[at];
        if (candidate->block < *block || (candidate->block == *block && candidate->edge < ordinal)) first = at + 1;
        else end = at;
    }
    if (first == homes->edge_count || !edge->argument_count) return false;
    const XrLirHomeEdge *move = &homes->edges[first];
    if (move->block != *block || move->edge != ordinal || !move->direct_header_move || move->target_block != edge->target)
        return false;
    XrLirHomeTransfer transport = homes->transfers[move->first];
    if (transport.source != carry->text) return false;
    const XrLirBlockData *target = &function->blocks[edge->target - 1];
    if (target->parameter_count != edge->argument_count) return false;
    bool has_suffix = false;
    uint32_t scalars = 0;
    for (uint32_t n = 0; n < edge->argument_count; ++n) {
        uint32_t argument = edge->arguments[n], parameter = target->parameters[n];
        if (parameter == transport.target) continue;
        if (carry->suffix && homes->values[argument].stationary_owner == carry->suffix &&
            xr_lir_home_stationary_edge(homes, argument, parameter)) {
            if (has_suffix) return false;
            has_suffix = true;
            continue;
        }
        if (!lir_string_window_scalar(function->values[argument - 1].type) ||
            !lir_string_window_scalar(function->values[parameter - 1].type)) return false;
        ++scalars;
    }
    if (has_suffix != (carry->suffix != 0)) return false;
    carry->replay |= scalars > 1;
    carry->edge = first;
    carry->text = transport.target;
    const XrLirValueHome *home = &homes->values[carry->text];
    XrLirStringReplayBlock row = {*block, ordinal, transport.target};
    *block = edge->target;
    return lir_home_append(b, carry->route, &row) &&
        lir_home_append(b, carry->slots, &home->temporary_first_owner) &&
        (*block == carry->header || lir_home_append(b, carry->slots, &home->first_owner));
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

/* Trace a scalar value through actual selected arguments, stopping at the
 * header recurrence. An argument can appear in more than one parallel phi. */
static uint32_t lir_string_window_origin(const XrLirFunctionData *f, uint32_t header,
    const XrLirStringReplayBlock *route, size_t count, uint32_t value) {
    while (count) {
        const XrLirValueData *v = &f->values[value - 1];
        if (!v->parameter || !v->block || v->block == header) break;
        const XrLirStringReplayBlock *row = &route[--count];
        const XrLirEdgeView *edge = lir_string_window_edge(f, row->block, row->edge);
        if (edge->target == v->block) value = edge->arguments[v->instruction];
    }
    return value;
}

typedef struct LirStringWindowBody {
    const XrLirInstructionView *literal, *append, *drop;
    uint32_t block, literal_at, append_at, drop_at;
} LirStringWindowBody;

static bool lir_string_window_body(LirHomeBuilder *b, const XrLirFunctionData *f,
    uint32_t id, LirStringWindowBody *body) {
    const XrLirBlockData *block = &f->blocks[id - 1];
    for (uint32_t n = 0; n + 1 < block->instruction_count; ++n) {
        const XrLirInstructionView *op = &block->instructions[n].view;
        if (xr_lir_internal_loop_scalar_operation(f, op)) continue;
        if (!body->literal && op->op == XR_LIR_STRING_LITERAL) {
            body->literal = op;
            body->literal_at = n + 1;
        } else if (!body->append && xr_lir_string_role(b->compiler.input, op) == XR_LIR_STRING_OP_APPEND_OWNED) {
            body->append = op;
            body->append_at = n + 1;
        } else if (!body->drop && op->op == XR_LIR_STRING_DROP) {
            body->drop = op;
            body->drop_at = n + 1;
        } else return false;
    }
    if (!body->append) return !body->literal && !body->drop;
    body->block = id;
    return true;
}

/* The backing can be updated in bulk only when no original scalar action
 * observes it. The replay itinerary retains the original evaluation order. */
static bool lir_home_string_window(LirHomeBuilder *b, const XrLirFunctionData *function, uint32_t header_id,
    XrLirStringWindow *out, size_t *latch_edge, LirHomeVector *slots, LirHomeVector *route, uint8_t **predecessors) {
    const XrLirBlockData *header = &function->blocks[header_id - 1];
    if (header->parameter_count < 2 || header->instruction_count < 2) return false;
    const XrLirInstructionView *branch = &header->instructions[header->instruction_count - 1].view;
    uint32_t order[2] = {0}, selected = 0;
    const XrLirInstructionView *cmp = xr_lir_internal_branch_i64_lt(function, branch, selected, order);
    if (!cmp || !function->values[order[0] - 1].parameter || function->values[order[0] - 1].block != header_id) {
        selected = 1;
        cmp = xr_lir_internal_branch_i64_lt(function, branch, selected, order);
    }
    if (!cmp || function->values[cmp->result - 1].block != header_id || order[0] == order[1]) return false;
    uint32_t index = order[0], bound = order[1], owner = 0, stable = 0;
    const XrLirValueData *iv = &function->values[index - 1];
    if (!iv->parameter || iv->block != header_id) return false;
    uint32_t comparison = function->values[cmp->result - 1].instruction;
    if (!lir_string_window_header(function, header, comparison)) return false;

    const XrLirValueHomes *homes = b->homes;
    for (uint32_t p = 0; p < header->parameter_count; ++p) {
        uint32_t id = header->parameters[p];
        XrLirType type = function->values[id - 1].type;
        if (lir_string_window_scalar(type)) continue;
        if (type != XR_LIR_STRING) return false;
        uint32_t producer = homes->values[id].stationary_owner;
        if (producer && !stable) stable = producer;
        else if (!producer && !owner) owner = id;
        else return false;
    }
    if (!owner) return false;
    size_t first_empty = slots->count, first_route = route->count;
    LirStringWindowCarry carry = {.header=header_id,.text=owner,.suffix=stable,.slots=slots,.route=route};
    uint32_t block = header_id;
    if (!lir_string_window_follow(b, function, &block, selected, &carry)) return false;
    if (!*predecessors) *predecessors = lir_string_window_predecessors(function);
    if (!*predecessors)
        return lir_home_fail(&b->compiler, XR_LIR_VALUE_LAYOUT_OOM, "cannot count String window predecessors");
    if (!(*predecessors)[header_id - 1]) return false;

    LirStringWindowBody body = {0};
    size_t suffix_slot = SIZE_MAX, actual_latch = 0, latch_owner = SIZE_MAX, latch_target = SIZE_MAX;
    size_t latch_route = 0;
    bool first_return = true;
    while (block != header_id) {
        /* Only the first returning bridge can share an entrance from outside. */
        if (block == function->entry || ((!body.append || !first_return) && (*predecessors)[block - 1] != 1))
            return false;
        const XrLirBlockData *current = &function->blocks[block - 1];
        uint32_t edge = xr_lir_internal_known_edge(function, &current->instructions[current->instruction_count - 1].view);
        if (edge == UINT32_MAX) return false;
        LirStringWindowBody candidate = {0};
        if (!lir_string_window_body(b, function, block, &candidate)) return false;
        if (candidate.append) {
            if (body.append || candidate.append->operands[0] != carry.text) return false;
            body = candidate;
            const XrLirValueHome *suffix = &homes->values[body.append->operands[1]];
            const XrLirValueHome *result = &homes->values[body.append->result];
            if (stable) {
                if (body.literal || body.drop || suffix->stationary_owner != stable) return false;
                suffix_slot = suffix->first_owner;
            } else {
                if (!body.literal || !body.drop || body.append->operands[1] != body.literal->result ||
                    body.drop->operands[0] != body.literal->result || !suffix->literal_cache_borrow) return false;
                suffix_slot = suffix->literal_cache_owner;
            }
            if (suffix_slot == SIZE_MAX || result->temporary_first_owner == SIZE_MAX ||
                !lir_home_append(b, slots, &result->first_owner) ||
                !lir_home_append(b, slots, &result->temporary_first_owner)) return false;
            carry.text = body.append->result;
            latch_owner = result->first_owner;
            latch_route = route->count - first_route;
        } else {
            carry.replay |= xr_lir_internal_forward_edge(function, current) == UINT32_MAX;
            if (body.append) first_return = false;
        }
        if (!lir_string_window_follow(b, function, &block, edge, &carry)) return false;
        if (candidate.append) {
            actual_latch = carry.edge;
            latch_target = homes->values[carry.text].first_owner;
        }
    }
    if (!body.append || carry.text != owner) return false;

    size_t count = route->count - first_route;
    const XrLirStringReplayBlock *path = (const XrLirStringReplayBlock *)route->data + first_route;
    const XrLirEdgeView *back = lir_string_window_edge(function, path[count - 1].block, path[count - 1].edge);
    uint32_t update = lir_string_window_origin(function, header_id, path, count - 1, back->arguments[iv->instruction]);
    const XrLirInstructionView *add = xr_lir_internal_value_definition(function, update);
    if (!add || add->op != XR_LIR_ADD || add->type != XR_LIR_I64 || add->operand_count != 2) return false;
    uint32_t left = lir_string_window_origin(function, header_id, path, count - 1, add->operands[0]);
    uint32_t right = lir_string_window_origin(function, header_id, path, count - 1, add->operands[1]);
    uint32_t one = left == index ? right : left;
    if ((left != index && right != index) || !xr_lir_internal_i64_one(function, one)) return false;
    const XrLirInstructionView *limit = xr_lir_internal_value_definition(function, bound);
    bool constant_limit = limit && limit->op == XR_LIR_CONST, update_inside = false;
    for (size_t n = 0; n < count; ++n) {
        if (!constant_limit && function->values[bound - 1].block == path[n].block) return false;
        if (n && function->values[update - 1].block == path[n].block) update_inside = true;
    }
    if (!update_inside) return false;

    const XrLirEdgeView *latch = lir_string_window_edge(function, path[latch_route].block, path[latch_route].edge);
    uint32_t latch_index = index;
    if (latch_route + 1 < count)
        latch_index = lir_string_window_origin(function, header_id, path + latch_route + 1,
            count - latch_route - 2, back->arguments[iv->instruction]);
    const XrLirValueData *latch_phi = &function->values[latch_index - 1];
    if (!latch_phi->parameter || latch_phi->block != latch->target || latch->arguments[latch_phi->instruction] != update)
        latch_index = 0;
    const XrLirBlockData *append_body = &function->blocks[body.block - 1];
    for (uint32_t n = 0; n + 1 < append_body->instruction_count; ++n) {
        const XrLirInstructionView *op = &append_body->instructions[n].view;
        if (xr_lir_internal_loop_scalar_operation(function, op) && op->result != update && op->result != one)
            carry.replay = true;
    }
    carry.replay |= !latch_index || left != index || append_body->parameter_count != header->parameter_count ||
        function->values[update - 1].block != body.block ||
        function->values[one - 1].block != body.block;

    size_t latch_empty = slots->count, header_owner = homes->values[owner].first_owner;
    for (size_t n = first_empty; n < latch_empty; ++n) {
        size_t slot = ((const size_t *)slots->data)[n];
        if (slot == latch_owner) slot = header_owner;
        if (!lir_home_append(b, slots, &slot)) return false;
    }
    *out = (XrLirStringWindow){.header=header_id,.index=index,.next_index=update,.latch_index=latch_index,.limit=bound,
        .comparison=comparison,.constant_limit=constant_limit,.limit_bits=constant_limit ? limit->bits : 0,
        .header_owner=header_owner,.latch_owner=latch_owner,.latch_target_owner=latch_target,.suffix_owner=suffix_slot,
        .header_empty_first=first_empty,.latch_empty_first=latch_empty,.empty_count=latch_empty - first_empty};
    if (carry.replay) {
        out->replay_first = first_route;
        out->replay_count = count;
        out->replay_body = body.block;
        out->replay_literal = body.literal_at;
        out->replay_append = body.append_at;
        out->replay_drop = body.drop_at;
    }
    *latch_edge = actual_latch;
    return true;
}

static bool lir_home_string_windows(LirHomeBuilder *b) {
    if (!b->homes->owned_frame_needed || !b->edges.count) return true;
    const XrLirFunctionData *function = &b->compiler.input->functions[b->compiler.function.id - 1];
    LirHomeVector windows = {.stride=sizeof(XrLirStringWindow)};
    LirHomeVector slots = {.stride=sizeof(size_t)};
    LirHomeVector route = {.stride=sizeof(XrLirStringReplayBlock)};
    uint8_t *predecessors = NULL;
    for (uint32_t header = 1; header <= function->block_count; ++header) {
        XrLirStringWindow window;
        size_t latch = 0;
        size_t first = slots.count, first_route = route.count;
        if (!lir_home_string_window(b, function, header, &window, &latch, &slots, &route, &predecessors)) {
            slots.count = first;
            route.count = first_route;
            if (b->compiler.status != XR_LIR_VALUE_LAYOUT_OK) break;
            continue;
        }
        if (!lir_home_append(b, &windows, &window)) break;
        /* A nonconstant bound is live into the original comparison. The
         * closed header does not redefine it, and no skipped step changes it. */
        if (!window.replay_count) {
            route.count = first_route;
            ((XrLirHomeEdge *)b->edges.data)[latch].string_window = (uint32_t)windows.count;
        }
    }
    xr_free(predecessors);
    if (!slots.count) {
        xr_free(slots.data);
        slots.data = NULL;
    }
    if (!route.count) {
        xr_free(route.data);
        route.data = NULL;
    }
    b->homes->string_replay_blocks = route.data;
    b->homes->string_replay_block_count = route.count;
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
    return first < homes->string_window_count && homes->string_windows[first].header == header &&
        !homes->string_windows[first].replay_count ?
        &homes->string_windows[first] : NULL;
}
