/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_string_replay.inc.c - Physical projection of a proved String route.
 */

static bool rbc_lir_string_replay_prepare(RbcLirCompiler *c) {
    const XrLirValueHomes *homes = c->homes;
    if (!c->value_code || !homes || !homes->string_replay_block_count) return true;
    if ((size_t)c->view.block_count >= SIZE_MAX / sizeof(*c->string_replay_sources))
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "String replay source index exceeds address space");
    c->string_replay_sources = xr_calloc((size_t)c->view.block_count + 1, sizeof(*c->string_replay_sources));
    if (!c->string_replay_sources)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot index String replay source blocks");
    for (size_t n = 0; n < homes->string_replay_block_count; ++n) {
        uint32_t block = homes->string_replay_blocks[n].block;
        if (!block || block > c->view.block_count)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "String replay source block is out of range");
        c->string_replay_sources[block].selected = true;
    }
    for (size_t n = 0; n < homes->string_window_count; ++n) {
        const XrLirStringWindow *window = &homes->string_windows[n];
        if (!window->replay_count) continue;
        if (!window->replay_body || window->replay_body > c->view.block_count || !window->replay_append)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "String replay lost its append position");
        RbcLirStringReplaySource *body = &c->string_replay_sources[window->replay_body];
        if (body->effect_at[1] && (body->effect_at[0] != window->replay_literal ||
            body->effect_at[1] != window->replay_append || body->effect_at[2] != window->replay_drop))
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "String replay append positions conflict");
        body->effect_at[0] = window->replay_literal;
        body->effect_at[1] = window->replay_append;
        body->effect_at[2] = window->replay_drop;
    }
    return true;
}

static void rbc_lir_string_replay_record(RbcLirCompiler *c, uint32_t block, uint32_t instruction,
    bool terminator, size_t first) {
    RbcLirStringReplaySource *source = &c->string_replay_sources[block];
    if (terminator) source->body_end = first;
    for (size_t n = 0; n < 3; ++n) {
        if (source->effect_at[n] != instruction) continue;
        source->effects[n] = (RbcLirStringReplaySpan){first, c->code_count};
        source->effects_seen |= 1u << n;
    }
}

typedef struct RbcLirStringReplayBuilder {
    size_t words, copies, steps;
} RbcLirStringReplayBuilder;

static bool rbc_lir_replay_scalar_register(const RbcLirCompiler *c, uint32_t slot) {
    return slot < c->register_count && c->register_kinds[slot] == XR_RBC_BITS;
}

static bool rbc_lir_replay_scalar_spill(const RbcLirCompiler *c, uint32_t slot) {
    return slot >= c->reference_spill_count && slot < c->spill_count;
}

/* Replay consumes ordinary scalar words. Only forward jumps from comparison
 * materialization may remain within these straight-line instruction spans. */
static bool rbc_lir_replay_word(const RbcLirCompiler *c, size_t pc, size_t end) {
    uint32_t word = c->code[pc];
    XrRbcOp op = (XrRbcOp)(word & 255u);
    if (op == XR_RBC_JMP) {
        uint32_t forward = word >> 8;
        return forward <= XR_RBC_JUMP_MAX && forward <= end - pc - 1;
    }
    uint32_t a = (word >> 8) & 255u;
    uint32_t b = (word >> 16) & 255u;
    uint32_t last = word >> 24;
    if (!rbc_lir_replay_scalar_register(c, a)) return false;
    switch (op) {
    case XR_RBC_LOADI: return true;
    case XR_RBC_LOADK: return (word >> 16) < c->constant_count;
    case XR_RBC_SPILL: case XR_RBC_RELOAD: return rbc_lir_replay_scalar_spill(c, word >> 16);
    case XR_RBC_MOV: case XR_RBC_NEG: case XR_RBC_ADDI: case XR_RBC_SUBI: case XR_RBC_MULI:
        return rbc_lir_replay_scalar_register(c, b);
    case XR_RBC_ADD: case XR_RBC_SUB: case XR_RBC_MUL:
        return rbc_lir_replay_scalar_register(c, b) && rbc_lir_replay_scalar_register(c, last);
    case XR_RBC_LT: case XR_RBC_LE: case XR_RBC_EQ:
        if (!rbc_lir_replay_scalar_register(c, b)) return false;
        /* fall through */
    case XR_RBC_LTI: case XR_RBC_LEI: case XR_RBC_GTI: case XR_RBC_GEI: case XR_RBC_EQI:
        return pc + 1 < end && (c->code[pc + 1] & 255u) == XR_RBC_JMP;
    default:
        return false;
    }
}

static bool rbc_lir_replay_words(RbcLirCompiler *c, RbcLirStringReplayBuilder *builder, size_t first, size_t end) {
    if (first > end || end > c->code_count) return false;
    for (size_t pc = first; pc < end; ++pc) {
        if (!rbc_lir_replay_word(c, pc, end)) return false;
    }
    if (first == end) return true;
    XrRbcValueCode *code = c->value_code;
    size_t length = end - first, previous = code->string_replay_word_count;
    if (length > SIZE_MAX - previous)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "String replay words exceed address space");
    void *storage = (void *)code->string_replay_words;
    if (!rbc_lir_grow(c, &storage, &builder->words, previous + length, sizeof(uint32_t))) return false;
    code->string_replay_words = storage;
    memcpy((uint32_t *)storage + previous, c->code + first, length * sizeof(uint32_t));
    code->string_replay_word_count = previous + length;
    return true;
}

static bool rbc_lir_replay_body(RbcLirCompiler *c, RbcLirStringReplayBuilder *builder,
    uint32_t block, const RbcLirStringReplaySource *source) {
    size_t first = c->blocks[block];
    /* Remove only the effect positions supplied by the shared proof, in their
     * actual instruction order. A copied local jump must stay within its span. */
    uint32_t pending = source->effects_seen;
    while (pending) {
        size_t next = 3;
        for (size_t n = 0; n < 3; ++n) {
            if ((pending & (1u << n)) && (next == 3 || source->effect_at[n] < source->effect_at[next]))
                next = n;
        }
        XR_DCHECK(next < 3, "String replay records only three append effects");
        pending &= ~(1u << next);
        RbcLirStringReplaySpan span = source->effects[next];
        if (span.end > source->body_end || !rbc_lir_replay_words(c, builder, first, span.first)) return false;
        first = span.end;
    }
    return rbc_lir_replay_words(c, builder, first, source->body_end);
}

static bool rbc_lir_replay_location(const RbcLirCompiler *c, uint32_t value, XrRbcValueLocation *out) {
    const XrRbcValueCode *code = c->value_code;
    const XrLirValueHomes *homes = c->homes;
    if (!value || value > code->value_count || homes->values[value].type ||
        homes->values[value].borrow_source || (homes->bindings && homes->bindings[value].type)) return false;
    XrRbcValueLocation where = code->locations[value];
    if (where.kind == XR_RBC_VALUE_REGISTER ? !rbc_lir_replay_scalar_register(c, where.index) :
        where.kind != XR_RBC_VALUE_SPILL || !rbc_lir_replay_scalar_spill(c, where.index)) return false;
    *out = where;
    return true;
}

static bool rbc_lir_replay_copies(RbcLirCompiler *c, RbcLirStringReplayBuilder *builder,
    XrRbcValueAccess access, uint32_t owner_parameter) {
    XrRbcValueCode *code = c->value_code;
    const uint32_t *pair = code->arguments + access.first;
    bool owner = false;
    for (uint32_t n = 0; n < access.count; ++n, pair += 2) {
        if (pair[1] == owner_parameter) {
            if (owner) return false;
            owner = true;
            continue;
        }
        XrRbcStringReplayCopy copy;
        if (!rbc_lir_replay_location(c, pair[0], &copy.source) ||
            !rbc_lir_replay_location(c, pair[1], &copy.target)) return false;
        if (copy.source.kind == copy.target.kind && copy.source.index == copy.target.index) continue;
        size_t next = code->string_replay_copy_count;
        if (next == SIZE_MAX)
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "String replay copies exceed address space");
        void *storage = (void *)code->string_replay_copies;
        if (!rbc_lir_grow(c, &storage, &builder->copies, next + 1, sizeof(copy))) return false;
        code->string_replay_copies = storage;
        ((XrRbcStringReplayCopy *)storage)[next] = copy;
        code->string_replay_copy_count = next + 1;
    }
    return owner;
}

static bool rbc_lir_replay_step(RbcLirCompiler *c, RbcLirStringReplayBuilder *builder,
    const XrLirStringReplayBlock *row, uint32_t target_block, uint32_t *ordinal) {
    XrRbcValueCode *code = c->value_code;
    const XrLirValueHomes *homes = c->homes;
    const RbcLirStringReplaySource *source = &c->string_replay_sources[row->block];
    if (!row->edge || row->edge > 2) return false;
    uint32_t at = source->edge_accesses[row->edge - 1];
    if (!at || at > code->access_count) return false;
    XrRbcValueAccess access = code->accesses[at - 1];
    if (access.operation != XR_RBC_VALUE_EDGE || access.other || access.window ||
        access.index >= homes->edge_count || !access.count || access.first > code->argument_count ||
        access.count > (code->argument_count - access.first) / 2) return false;
    const XrLirHomeEdge *edge = &homes->edges[access.index];
    if (!edge->direct_header_move || edge->count != 1 || edge->first >= homes->transfer_count ||
        edge->block != row->block || edge->edge != row->edge || edge->target_block != target_block ||
        homes->transfers[edge->first].target != row->owner_parameter) return false;

    XrRbcStringReplayStep step = {.first_word=code->string_replay_word_count,
        .first_copy=code->string_replay_copy_count};
    if (!rbc_lir_replay_body(c, builder, row->block, source) ||
        !rbc_lir_replay_copies(c, builder, access, row->owner_parameter)) return false;
    step.word_count = code->string_replay_word_count - step.first_word;
    step.copy_count = code->string_replay_copy_count - step.first_copy;
    size_t next = code->string_replay_step_count;
    if (next == SIZE_MAX)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "String replay steps exceed address space");
    void *storage = (void *)code->string_replay_steps;
    if (!rbc_lir_grow(c, &storage, &builder->steps, next + 1, sizeof(step))) return false;
    code->string_replay_steps = storage;
    ((XrRbcStringReplayStep *)storage)[next] = step;
    code->string_replay_step_count = next + 1;
    *ordinal = at;
    return true;
}

static bool rbc_lir_string_replay_project(RbcLirCompiler *c) {
    if (!c->string_replay_sources) return true;
    XrRbcValueCode *code = c->value_code;
    const XrLirValueHomes *homes = c->homes;
    XrRbcStringReplay *plans = NULL;
    uint32_t *accesses = NULL;
    RbcLirStringReplayBuilder builder = {0};
    for (size_t w = 0; w < homes->string_window_count; ++w) {
        const XrLirStringWindow *window = &homes->string_windows[w];
        if (!window->replay_count) continue;
        if (window->replay_first > homes->string_replay_block_count ||
            window->replay_count > homes->string_replay_block_count - window->replay_first)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "String replay route is out of range");
        const XrLirStringReplayBlock *path = homes->string_replay_blocks + window->replay_first;
        if (path[0].block != window->header)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "String replay route must start at its header");
        XrRbcValueLocation guard_value;
        if (!rbc_lir_replay_location(c, window->index, &guard_value) ||
            (!window->constant_limit && !rbc_lir_replay_location(c, window->limit, &guard_value))) continue;
        const size_t first_step = code->string_replay_step_count, first_word = code->string_replay_word_count,
            first_copy = code->string_replay_copy_count;
        uint32_t latch_access = 0;
        size_t n = 0;
        for (; n < window->replay_count; ++n) {
            uint32_t target = n + 1 < window->replay_count ? path[n + 1].block : window->header;
            if (!rbc_lir_replay_step(c, &builder, path + n, target, &latch_access)) break;
        }
        if (n != window->replay_count) {
            if (c->status != XR_RBC_COMPILE_OK) return false;
            code->string_replay_step_count = first_step;
            code->string_replay_word_count = first_word;
            code->string_replay_copy_count = first_copy;
            continue;
        }
        if (!plans) {
            plans = xr_calloc(homes->string_window_count, sizeof(*plans));
            code->string_replays = plans;
            accesses = xr_calloc(code->access_count, sizeof(*accesses));
            code->string_replay_accesses = accesses;
            if (!plans || !accesses)
                return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own String replay entry index");
        }
        if (accesses[latch_access - 1] || w >= UINT32_MAX)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "String replay entry is ambiguous");
        plans[w] = (XrRbcStringReplay){first_step, n, latch_access};
        accesses[latch_access - 1] = (uint32_t)w + 1;
    }
    if (!plans) {
        xr_free((void *)code->string_replay_steps);
        xr_free((void *)code->string_replay_words);
        xr_free((void *)code->string_replay_copies);
        code->string_replay_steps = NULL;
        code->string_replay_words = NULL;
        code->string_replay_copies = NULL;
    }
    return true;
}
