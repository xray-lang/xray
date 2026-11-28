/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_array_hold.c - Local Arrays held between their aliases
 *
 * KEY CONCEPT:
 *   Verified LIR keeps every owner inside its defining block, passing it on
 *   only through edges. An Array chain is therefore the union of the owners a
 *   local Array takes: block parameters joined to their edge arguments, and
 *   the read-back of a private Cell round trip joined to the owner it copied.
 *   Holding a chain needs no region analysis: a held owner is unique and has
 *   no alias, so whatever runs between its uses cannot reach it. Each use is
 *   either a held operation or an alias that sees the unheld owner.
 */
#include "xlir_array_hold.h"
#include "xlir_array.h"
#include "xlir_rebuild_internal.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"

enum {
    HOLD_ROLE_NONE,
    HOLD_ROLE_GET,      /* an element read of a chain owner */
    HOLD_ROLE_LEN,      /* the length of a chain owner */
    HOLD_ROLE_COPY,     /* the Cell a round trip copies a chain owner into */
    HOLD_ROLE_RELEASE,  /* the release of the copied owner */
    HOLD_ROLE_MUTATE,   /* a set or push through the round trip's Cell */
    HOLD_ROLE_READ,     /* the read-back that continues the chain */
    HOLD_ROLE_RETIRE    /* the release of the round trip's Cell */
};

typedef struct {
    const XrLirModule *input;
    XrLirFunction function;
    XrLirFunctionView view;
    size_t *first;        /* flat offset of each one-based block's instructions */
    uint8_t *role;        /* flat instruction roles */
    uint32_t *source;     /* flat: the chain owner a round-trip instruction stands for */
    uint32_t *parent;     /* union-find over values; zero marks a non-chain value */
    uint8_t *borrowed;    /* values that are incoming parameters */
    uint8_t *uses;        /* per root: held operations or round trips exist */
    uint8_t *mutated;     /* per root: a round trip mutates the chain */
    uint32_t *unheld;     /* values the current instruction sees unheld */
    uint32_t unheld_count;
    uint32_t operands[3]; /* rewritten operands of the instruction being copied */
} Hold;

static void *hold_storage(uint64_t count, size_t size) {
    return count && count <= SIZE_MAX / size ? xr_calloc((size_t)count, size) : NULL;
}
static void hold_end(void *context) {
    Hold *c = context;
    xr_free(c->first); xr_free(c->role); xr_free(c->source); xr_free(c->parent);
    xr_free(c->borrowed); xr_free(c->uses); xr_free(c->mutated); xr_free(c->unheld);
    *c = (Hold){0};
}
static XrLirInstructionView hold_view(const Hold *c, uint32_t block, uint32_t ordinal) {
    XrLirInstructionView view;
    xr_lir_instruction_view(c->input, (XrLirBlock){c->input, c->function.id, block}, ordinal, &view);
    return view;
}
static uint32_t hold_find(Hold *c, uint32_t value) {
    while (c->parent[value] != value) value = c->parent[value] = c->parent[c->parent[value]];
    return value;
}
static void hold_union(Hold *c, uint32_t a, uint32_t b) {
    if (!c->parent[a] || !c->parent[b]) return;
    a = hold_find(c, a); b = hold_find(c, b);
    if (a == b) return;
    c->parent[b] = a;
    c->uses[a] |= c->uses[b]; c->mutated[a] |= c->mutated[b]; c->borrowed[a] |= c->borrowed[b];
}
/* Whether a root's chain is held. */
static bool hold_chain(Hold *c, uint32_t value) {
    if (!value || value > c->view.value_count || !c->parent[value]) return false;
    uint32_t root = hold_find(c, value);
    return c->uses[root] && !c->borrowed[root];
}
static int hold_array_operation(const Hold *c, const XrLirInstructionView *v) {
    if (v->op != XR_LIR_CALL_INTRINSIC) return 0;
    XrLirArrayIntrinsic shape = {0};
    return xr_lir_array_intrinsic_view(c->input, v->intrinsic, &shape) == XR_LIR_OK && shape.id ?
        (int)shape.operation : 0;
}

static XrLirStatus hold_allocate(Hold *c) {
    uint64_t values = (uint64_t)c->view.value_count + 1;
    c->first = hold_storage((uint64_t)c->view.block_count + 1, sizeof(*c->first));
    c->parent = hold_storage(values, sizeof(*c->parent));
    c->borrowed = hold_storage(values, sizeof(*c->borrowed));
    c->uses = hold_storage(values, sizeof(*c->uses));
    c->mutated = hold_storage(values, sizeof(*c->mutated));
    c->unheld = hold_storage(values, sizeof(*c->unheld));
    if (!c->first || !c->parent || !c->borrowed || !c->uses || !c->mutated || !c->unheld) return XR_LIR_OUT_OF_MEMORY;
    uint64_t total = 0;
    for (uint32_t b = 1; b <= c->view.block_count; ++b) {
        XrLirBlockView view;
        xr_lir_block_view(c->input, (XrLirBlock){c->input, c->function.id, b}, &view);
        c->first[b] = (size_t)total;
        total += view.instruction_count;
    }
    c->role = hold_storage(total, sizeof(*c->role));
    c->source = hold_storage(total, sizeof(*c->source));
    if (!c->role || !c->source) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        XrLirValue value = {c->input, c->function.id, v};
        XrLirType type = XR_LIR_TYPE_INVALID;
        XrLirArrayShape shape = {0};
        if (xr_lir_value_type(c->input, value, &type) == XR_LIR_OK && type == XR_LIR_ARRAY &&
            xr_lir_array_shape_view(c->input, value, &shape) == XR_LIR_OK && xr_lir_array_holdable(shape.element))
            c->parent[v] = v;
    }
    for (uint32_t p = 0; p < c->view.parameter_count; ++p) c->borrowed[c->view.parameters[p]] = 1;
    return XR_LIR_OK;
}

/* A private round trip: a Cell copied from a chain owner whose source is then
 * released, mutated only by element sets and pushes, read back once and
 * released, all in this block and in that order. */
static void hold_round_trip(Hold *c, uint32_t block, uint32_t count, uint32_t at, const XrLirInstructionView *copy) {
    uint32_t owner = copy->operands[0], cell = copy->result;
    if (!c->parent[owner]) return;
    uint32_t release = 0, read = 0, retire = 0, mutations = 0;
    for (uint32_t n = at + 1; n < count; ++n) {
        XrLirInstructionView v = hold_view(c, block, n);
        for (uint32_t e = 0; e < v.edge_count; ++e)
            for (uint32_t a = 0; a < v.edges[e].argument_count; ++a)
                if (v.edges[e].arguments[a] == cell || v.edges[e].arguments[a] == owner) return;
        bool uses_cell = false, uses_owner = false;
        for (uint32_t a = 0; a < v.operand_count; ++a) {
            uses_cell |= v.operands[a] == cell;
            uses_owner |= v.operands[a] == owner;
        }
        if (uses_owner) {
            if (release || v.op != XR_LIR_ARRAY_DROP || mutations || read) return;
            release = n + 1;
            continue;
        }
        if (!uses_cell) continue;
        int operation = hold_array_operation(c, &v);
        if (!read && (operation == XR_LIR_ARRAY_OP_SET || operation == XR_LIR_ARRAY_OP_PUSH) &&
            v.operands[0] == cell) {
            for (uint32_t a = 1; a < v.operand_count; ++a) if (v.operands[a] == cell) return;
            ++mutations;
            continue;
        }
        if (!read && v.op == XR_LIR_CELL_READ && v.type == XR_LIR_ARRAY && release) { read = n + 1; continue; }
        if (read && !retire && v.op == XR_LIR_ARRAY_CELL_DROP) { retire = n + 1; break; }
        return;
    }
    if (!release || !read || !retire || !mutations) return;
    uint32_t chain_read = hold_view(c, block, read - 1).result;
    if (!c->parent[chain_read]) return;
    size_t base = c->first[block];
    c->role[base + at] = HOLD_ROLE_COPY;
    c->role[base + release - 1] = HOLD_ROLE_RELEASE;
    c->role[base + read - 1] = HOLD_ROLE_READ;
    c->role[base + retire - 1] = HOLD_ROLE_RETIRE;
    c->source[base + at] = c->source[base + release - 1] = c->source[base + read - 1] =
        c->source[base + retire - 1] = owner;
    for (uint32_t n = at + 1; n < read - 1; ++n) {
        XrLirInstructionView v = hold_view(c, block, n);
        if (v.operand_count && v.operands[0] == cell) { c->role[base + n] = HOLD_ROLE_MUTATE; c->source[base + n] = owner; }
    }
    hold_union(c, owner, chain_read);
    uint32_t root = hold_find(c, owner);
    c->uses[root] = c->mutated[root] = 1;
}

static void hold_block(Hold *c, uint32_t block) {
    XrLirBlockView view;
    xr_lir_block_view(c->input, (XrLirBlock){c->input, c->function.id, block}, &view);
    for (uint32_t n = 0; n < view.instruction_count; ++n) {
        XrLirInstructionView v = hold_view(c, block, n);
        if (c->role[c->first[block] + n]) continue;
        if (v.op == XR_LIR_CELL_NEW && v.type == XR_LIR_ARRAY_CELL && v.operand_count == 1) {
            hold_round_trip(c, block, view.instruction_count, n, &v);
            continue;
        }
        int operation = hold_array_operation(c, &v);
        if ((operation == XR_LIR_ARRAY_OP_GET || operation == XR_LIR_ARRAY_OP_LEN) && v.operand_count &&
            c->parent[v.operands[0]]) {
            c->role[c->first[block] + n] = operation == XR_LIR_ARRAY_OP_GET ? HOLD_ROLE_GET : HOLD_ROLE_LEN;
            c->uses[hold_find(c, v.operands[0])] = 1;
        }
        for (uint32_t e = 0; e < v.edge_count; ++e) {
            XrLirBlockView target;
            xr_lir_block_view(c->input, (XrLirBlock){c->input, c->function.id, v.edges[e].target}, &target);
            uint32_t offset = xr_lir_edge_parameter_offset(&v, e);
            for (uint32_t a = 0; a < v.edges[e].argument_count; ++a)
                hold_union(c, v.edges[e].arguments[a], target.parameters[offset + a]);
        }
    }
}

static XrLirStatus hold_begin(void *context, const XrLirModule *input, XrLirFunction function) {
    Hold *c = context;
    XR_DCHECK(!c->first && !c->role, "the preceding function released its scratch");
    c->input = input;
    c->function = function;
    xr_lir_function_view(input, function, &c->view);
    XrLirStatus status = hold_allocate(c);
    if (status != XR_LIR_OK) return status;
    for (uint32_t b = 1; b <= c->view.block_count; ++b) hold_block(c, b);
    return XR_LIR_OK;
}
static bool hold_keep(void *context, uint32_t block) {
    (void)context; (void)block;
    return true;
}
static bool hold_held_parameter(void *context, uint32_t block, uint32_t parameter) {
    (void)block;
    return hold_chain(context, parameter);
}

static void hold_project(void *context, uint32_t block, uint32_t ordinal, XrLirInstructionView *v) {
    Hold *c = context;
    size_t at = c->first[block] + ordinal;
    uint8_t role = c->role[at];
    uint32_t owner = role == HOLD_ROLE_GET || role == HOLD_ROLE_LEN ? v->operands[0] : c->source[at];
    if (!role || !hold_chain(c, owner)) return;
    switch (role) {
    case HOLD_ROLE_GET: v->op = XR_LIR_ARRAY_HELD_GET; v->intrinsic = 0; break;
    case HOLD_ROLE_LEN: v->op = XR_LIR_ARRAY_HELD_LEN; v->intrinsic = 0; break;
    case HOLD_ROLE_COPY: v->op = XR_LIR_OP_INVALID; v->operand_count = 0; break;
    case HOLD_ROLE_RELEASE: v->op = XR_LIR_OP_INVALID; break;
    case HOLD_ROLE_MUTATE: {
        int operation = hold_array_operation(c, v);
        for (uint32_t a = 0; a < v->operand_count; ++a) c->operands[a] = a ? v->operands[a] : owner;
        v->op = operation == XR_LIR_ARRAY_OP_SET ? XR_LIR_ARRAY_HELD_SET : XR_LIR_ARRAY_HELD_PUSH;
        v->intrinsic = 0;
        v->operands = c->operands;
        break;
    }
    case HOLD_ROLE_READ: c->operands[0] = owner; v->operands = c->operands; v->op = XR_LIR_OP_INVALID; break;
    case HOLD_ROLE_RETIRE: v->op = XR_LIR_OP_INVALID; break;
    default: break;
    }
}

/* Before an alias use the instruction sees the unheld owner; after it, an
 * owner it did not retire is held again, and a newly produced chain owner is
 * held at once. Round trips and held operations need neither. */
static XrLirStatus hold_around(void *context, XrLirCopyEmitter *emitter, uint32_t block, uint32_t ordinal, bool after) {
    Hold *c = context;
    size_t at = c->first[block] + ordinal;
    XrLirInstructionView v = hold_view(c, block, ordinal);
    if (!after) {
        c->unheld_count = 0;
        if (c->role[at]) return XR_LIR_OK;
        for (uint32_t a = 0; a < v.operand_count; ++a) {
            uint32_t value = v.operands[a];
            if (!hold_chain(c, value)) continue;
            bool seen = false;
            for (uint32_t u = 0; u < c->unheld_count; ++u) seen |= c->unheld[u] == value;
            if (seen) continue;
            XrLirStatus status = xr_lir_copy_emit(emitter, XR_LIR_ARRAY_UNHOLD, XR_LIR_ARRAY, 0, &value, 1, value);
            if (status != XR_LIR_OK) return status;
            c->unheld[c->unheld_count++] = value;
        }
        return XR_LIR_OK;
    }
    bool retires = v.op == XR_LIR_ARRAY_DROP;
    for (uint32_t u = 0; u < c->unheld_count && !retires; ++u) {
        uint32_t value = c->unheld[u];
        XrLirStatus status = xr_lir_copy_emit(emitter, XR_LIR_ARRAY_HOLD, XR_LIR_ARRAY,
            c->mutated[hold_find(c, value)], &value, 1, value);
        if (status != XR_LIR_OK) return status;
    }
    c->unheld_count = 0;
    if (v.result && c->role[at] != HOLD_ROLE_READ && hold_chain(c, v.result))
        return xr_lir_copy_emit(emitter, XR_LIR_ARRAY_HOLD, XR_LIR_ARRAY, c->mutated[hold_find(c, v.result)],
            &v.result, 1, v.result);
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_array_hold(const XrLirModule *input, XrLirModule **output) {
    Hold c = {0};
    const XrLirCopyPlan plan = {&c, hold_begin, hold_keep, hold_project, hold_end, hold_around, hold_held_parameter};
    return xr_lir_internal_copy(input, output, &plan);
}
