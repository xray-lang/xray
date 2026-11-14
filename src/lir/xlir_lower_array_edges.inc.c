/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_array_edges.inc.c - Retire live-out owners on the actually selected edge
 */
static bool lower_edge_copy(LowerFunction *f, XrLirBlock block, XrLirValue value, XrLirValue *out) {
    XrLirType type = lower_value_type(f, value);
    XrLirInstruction copy = {.op = lower_retain_operation(type),
        .type = type, .operands = &value, .operand_count = 1, .source = lower_source(f->owner)};
    return lower_build(f->owner, xr_lir_emit_value(f->owner->module, block, &copy, out));
}
static bool lower_edge_owners(LowerFunction *f, XrLirBlock block, XrLirEdge *edge) {
    XrLirValue *args = (XrLirValue *)edge->arguments;
    bool managed = false;
    for (uint32_t a = 0; a < edge->argument_count; ++a) managed |= lower_managed_type(f->owner, lower_value_type(f, args[a]));
    if (!managed) return true;
    uint32_t values = f->owner->module->functions[f->function.id - 1].value_count;
    uint8_t *seen = lower_alloc(f->owner, values, 1);
    if (values && !seen) return false;
    bool good = true;
    for (uint32_t a = 0; a < edge->argument_count; ++a) {
        const XrLirValueData *value = &f->owner->module->functions[f->function.id - 1].values[args[a].id - 1];
        if (!lower_managed_type(f->owner, value->type)) continue;
        bool copy = (value->parameter && !value->block) || seen[args[a].id - 1];
        seen[args[a].id - 1] = 1;
        if (copy) {
            XrLirValue retained = {0};
            if (!lower_edge_copy(f, block, args[a], &retained)) { good = false; break; }
            args[a] = retained;
        }
    }
    xr_free(seen);
    return good;
}

typedef struct LowerEdgeOwners {
    const XrLirValue *values;
    const uint32_t *positions;
    uint32_t count;
} LowerEdgeOwners;

static bool lower_selected_edge(LowerFunction *f, uint32_t from, uint32_t ordinal,
    const LowerEdgeOwners *owners, XrLirInstruction *term) {
    Lower *c = f->owner;
    XrLirEdge *edge = &term->edges[ordinal];
    uint32_t count = owners->count;
    XrLirType *types = lower_alloc(c, count, sizeof(*types));
    XrLirValue *parameters = lower_alloc(c, count, sizeof(*parameters));
    XrLirValue *arguments = lower_alloc(c, edge->argument_count, sizeof(*arguments));
    XrLirValue *incoming = lower_alloc(c, count, sizeof(*incoming));
    uint8_t *transferred = lower_alloc(c, count, 1);
    bool good = (!count || (types && parameters && incoming && transferred)) && (!edge->argument_count || arguments);
    XrLirBlock block = {0};
    for (uint32_t i = 0; i < count && good; ++i) {
        types[i] = lower_value_type(f, owners->values[i]);
        incoming[i] = owners->values[i];
    }
    if (good) good = lower_build(c, xr_lir_add_block(c->module, f->function, types, count, &block));
    for (uint32_t i = 0; i < count && good; ++i) {
        good = lower_build(c, xr_lir_block_parameter(c->module, block, i, &parameters[i]));
        if (good && types[i] == XR_LIR_ARRAY) {
            XrLirArrayShape shape = {c->module->functions[f->function.id - 1].values[owners->values[i].id - 1].array_element};
            good = lower_build(c, xr_lir_set_array_shape(c->module, parameters[i], &shape));
        }
    }
    if (good) good = lower_array_cells_release(f, from, term, ordinal, block);
    for (uint32_t a = 0; a < edge->argument_count && good; ++a) {
        arguments[a] = edge->arguments[a];
        uint32_t position = owners->positions[arguments[a].id - 1];
        if (position) {
            arguments[a] = parameters[position - 1];
            transferred[position - 1] = 1;
        }
    }
    XrLirEdge selected = {edge->target, arguments, edge->argument_count};
    if (good) good = lower_edge_owners(f, block, &selected);
    for (uint32_t i = 0; i < count && good; ++i) {
        if (!transferred[i]) {
            XrLirInstruction drop = {.op = lower_drop_operation(types[i]),
                .type = XR_LIR_UNIT, .operands = &parameters[i], .operand_count = 1, .source = lower_source(c)};
            good = lower_build(c, xr_lir_emit_effect(c->module, block, &drop));
        }
    }
    XrLirInstruction branch = {.op = XR_LIR_BR, .type = XR_LIR_UNIT, .edge_count = 1,
        .edges = {selected}, .source = lower_source(c)};
    if (good) good = lower_build(c, xr_lir_set_terminator(c->module, block, &branch));
    if (good) {
        xr_free((void *)edge->arguments);
        *edge = (XrLirEdge){block, incoming, count};
        incoming = NULL;
    }
    xr_free(transferred); xr_free(incoming); xr_free(arguments); xr_free(parameters); xr_free(types);
    return good;
}

static bool lower_selected_edges(LowerFunction *f, uint32_t b, XrLirInstruction *term) {
    if (term->edge_count < 2)
        return !term->edge_count || lower_edge_owners(f, f->blocks[b], &term->edges[0]);
    Lower *c = f->owner;
    bool managed = false, cells = false;
    for (uint32_t e = 0; e < term->edge_count; ++e)
        cells |= lower_array_cells_edge(f, b, e);
    for (uint32_t e = 0; e < term->edge_count; ++e)
        for (uint32_t a = 0; a < term->edges[e].argument_count; ++a)
            managed |= lower_managed_type(f->owner, lower_value_type(f, term->edges[e].arguments[a]));
    if (!managed && !cells) return true;
    size_t capacity = (size_t)term->edges[0].argument_count + term->edges[1].argument_count;
    if (capacity < term->edges[0].argument_count) {
        lower_error(c, XR_LIR_OUT_OF_MEMORY, "selected edge owner count overflow"); return false;
    }
    XrLirValue *owners = lower_alloc(c, capacity, sizeof(*owners));
    uint32_t values = c->module->functions[f->function.id - 1].value_count;
    uint32_t *positions = lower_alloc(c, values, sizeof(*positions));
    if ((capacity && !owners) || (values && !positions)) { xr_free(owners); xr_free(positions); return false; }
    uint32_t count = 0;
    for (uint32_t e = 0; e < term->edge_count; ++e)
        for (uint32_t a = 0; a < term->edges[e].argument_count; ++a) {
            XrLirValue id = term->edges[e].arguments[a];
            const XrLirValueData *value = &c->module->functions[f->function.id - 1].values[id.id - 1];
            if (!lower_managed_type(f->owner, value->type)) continue;
            if (value->parameter && !value->block) continue;
            if (!positions[id.id - 1]) {
                owners[count] = id;
                positions[id.id - 1] = ++count;
            }
        }
    bool good = true;
    LowerEdgeOwners map = {owners, positions, count};
    for (uint32_t e = 0; e < term->edge_count && good; ++e)
        if (managed || lower_array_cells_edge(f, b, e))
            good = lower_selected_edge(f, b, e, &map, term);
    xr_free(positions); xr_free(owners);
    return good;
}
