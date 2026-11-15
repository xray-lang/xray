/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_facts.inc.c - Constructor-owned callable and scope snapshots
 */
typedef struct LirCallableFunctionFacts {
    XrLirCallableFunctionFacts view;
    size_t bytes;
} LirCallableFunctionFacts;

struct XrLirCallableFacts {
    uint32_t function_count;
    LirCallableFunctionFacts **functions;
};

typedef struct LirCallableGeometry {
    const uint32_t *parents, *owners, *floors;
} LirCallableGeometry;

_Static_assert(sizeof(LirCallableFunctionFacts) % _Alignof(XrLirCallableValueFact) == 0 &&
    sizeof(XrLirCallableValueFact) % _Alignof(uint32_t) == 0 &&
    sizeof(XrLirCallableFacts) % _Alignof(LirCallableFunctionFacts *) == 0,
    "one-block callable descriptions preserve natural row and capture alignment");

XR_FUNC void xr_lir_callable_facts_destroy(XrLirCallableFacts *facts) {
    if (!facts) return;
    for (uint32_t f = 0; f < facts->function_count; ++f) xr_free(facts->functions[f]);
    xr_free(facts);
}

static XrLirCallableFacts *lir_callable_facts_allocate(uint32_t functions) {
    if (functions > (SIZE_MAX - sizeof(XrLirCallableFacts)) / sizeof(LirCallableFunctionFacts *)) return NULL;
    XrLirCallableFacts *facts = xr_calloc(1,
        sizeof(*facts) + (size_t)functions * sizeof(LirCallableFunctionFacts *));
    if (facts) {
        facts->function_count = functions;
        facts->functions = (LirCallableFunctionFacts **)(facts + 1);
    }
    return facts;
}

static bool lir_function_has_callables(const XrLirModule *m, const XrLirFunctionData *f) {
    XrLirCallableType signature;
    if (xr_lir_callable_type_view(m, f->result, &signature) == XR_LIR_OK) return true;
    for (uint32_t v = 0; v < f->value_count; ++v)
        if (f->values[v].type == XR_LIR_FUNCTION_SCOPE ||
            xr_lir_callable_type_view(m, f->values[v].type, &signature) == XR_LIR_OK) return true;
    return false;
}

static LirCallableFunctionFacts *lir_callable_function_allocate(uint32_t values, uint32_t captures) {
    size_t rows = (size_t)values + 1;
    if (!rows || rows > (SIZE_MAX - sizeof(LirCallableFunctionFacts)) / sizeof(XrLirCallableValueFact)) return NULL;
    size_t bytes = sizeof(LirCallableFunctionFacts) + rows * sizeof(XrLirCallableValueFact);
    if (captures > (SIZE_MAX - bytes) / sizeof(uint32_t)) return NULL;
    bytes += (size_t)captures * sizeof(uint32_t);
    LirCallableFunctionFacts *facts = xr_calloc(1, bytes);
    if (!facts) return NULL;
    facts->bytes = bytes;
    facts->view = (XrLirCallableFunctionFacts){
        .values=(XrLirCallableValueFact *)(facts + 1), .value_count=values,
        .captures=captures ? (uint32_t *)((XrLirCallableValueFact *)(facts + 1) + rows) : NULL,
        .capture_count=captures};
    return facts;
}

/* Links are compressed once. Scope geometry and constructor captures are
 * copied, not recomputed along each alias chain. */
static bool lir_callable_facts_resolve(XrLirCallableValueFact *rows, uint32_t count,
    uint32_t *links, uint32_t *path) {
    for (uint32_t v = 1; v <= count; ++v) {
        if (!rows[v].type || !links[v]) continue;
        uint32_t current = v, depth = 0;
        while (links[current] && links[current] != UINT32_MAX) {
            uint32_t next = links[current];
            if (!next || next > count || !rows[next].type || depth == count) return false;
            path[depth++] = current; links[current] = UINT32_MAX; current = next;
        }
        bool persistent = links[current] == UINT32_MAX ? false : rows[current].persistent;
        uint32_t origin = links[current] == UINT32_MAX ? 0 : rows[current].origin;
        while (depth) {
            uint32_t value = path[--depth];
            rows[value].persistent = persistent; rows[value].origin = origin;
            rows[value].known_target = origin ? rows[origin].known_target : 0;
            links[value] = 0;
        }
    }
    return true;
}

static XrLirStatus lir_callable_facts_capture(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, LirCallableGeometry geometry, XrLirCallableFacts **output) {
    if (!output || !lir_function_has_callables(m, f)) return XR_LIR_OK;
    uint32_t captures = 0;
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        const XrLirInstructionView *op = xr_lir_internal_value_definition(f, v);
        if (!op || op->op != XR_LIR_FUNCTION_REF) continue;
        if (op->operand_count > UINT32_MAX - captures) return XR_LIR_OUT_OF_MEMORY;
        captures += op->operand_count;
    }
    if (!*output) *output = lir_callable_facts_allocate(m->function_count);
    if (!*output) return XR_LIR_OUT_OF_MEMORY;
    LirCallableFunctionFacts *received = lir_callable_function_allocate(f->value_count, captures);
    size_t count = (size_t)f->value_count + 1;
    uint32_t *scratch = count && count <= SIZE_MAX / (2 * sizeof(uint32_t)) ?
        xr_calloc(count * 2, sizeof(uint32_t)) : NULL;
    if (!received || !scratch) { xr_free(received); xr_free(scratch); return XR_LIR_OUT_OF_MEMORY; }
    XrLirCallableValueFact *rows = (XrLirCallableValueFact *)received->view.values;
    uint32_t *captured = (uint32_t *)received->view.captures, *links = scratch, *path = scratch + count;
    uint32_t contract = 0;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        rows[f->parameters[p]].parameter = p + 1;
        rows[f->parameters[p]].persistent = lir_callable_parameter_persistent(f, p, &contract);
    }
    uint32_t next_capture = 0;
    for (uint32_t v = 1; v <= f->value_count; ++v) {
        XrLirCallableType signature;
        bool callable = xr_lir_callable_type_view(m, f->values[v - 1].type, &signature) == XR_LIR_OK;
        if (!callable && f->values[v - 1].type != XR_LIR_FUNCTION_SCOPE) {
            rows[v] = (XrLirCallableValueFact){0}; continue;
        }
        XrLirCallableValueFact *row = &rows[v];
        row->type = f->values[v - 1].type; row->origin = v; row->frame_floor = UINT32_MAX;
        row->owner_scope = callable && geometry.owners ? geometry.owners[v] : 0;
        const XrLirInstructionView *op = xr_lir_internal_value_definition(f, v);
        if (op) {
            row->operation = op->op; row->block = f->values[v - 1].block;
            row->instruction = f->values[v - 1].instruction + 1;
            if (op->op == XR_LIR_FUNCTION_REF) {
                row->known_target = op->callee; row->constructor_storage = (uint32_t)op->bits;
                row->first_capture = op->operand_count ? next_capture : 0; row->capture_count = op->operand_count;
                row->state_capture = m->functions[op->callee - 1].closure.state_capture;
                if (op->operand_count) memcpy(captured + next_capture, op->operands, (size_t)op->operand_count * sizeof(uint32_t));
                next_capture += op->operand_count;
            } else if (op->op == XR_LIR_FUNCTION_BIND || op->op == XR_LIR_FUNCTION_SCOPE_LEAVE) {
                row->binding_identity = op->op == XR_LIR_FUNCTION_BIND ? op->bits : 0;
                row->receiving_scope = op->op == XR_LIR_FUNCTION_BIND ? op->operands[0] :
                    geometry.parents ? geometry.parents[op->operands[0]] : 0;
                row->frame_floor = geometry.floors ? geometry.floors[v] : UINT32_MAX;
            } else if (op->op == XR_LIR_FUNCTION_SCOPE_ENTER)
                row->receiving_scope = op->operand_count ? op->operands[0] : 0;
        }
        if (callable && !row->parameter) {
            row->persistent = lir_callable_persistence_seed(f, v, UINT32_MAX, &links[v]);
            row->alias_source = links[v];
        }
    }
    XR_DCHECK(next_capture == captures, "every constructor operand has one owned capture ordinal");
    bool resolved = lir_callable_facts_resolve(rows, f->value_count, links, path);
    xr_free(scratch);
    if (!resolved) { xr_free(received); return XR_LIR_VERIFY_ERROR; }
    XR_DCHECK(!(*output)->functions[function - 1], "each final function publishes one callable snapshot");
    (*output)->functions[function - 1] = received;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_callable_facts_create(const XrLirModule *m, XrLirCallableFacts **empty) {
    if (!empty || *empty || !xr_lir_module_is_readable(m)) return XR_LIR_INVALID_ARGUMENT;
    if (!m->callable_facts) return XR_LIR_OK;
    const XrLirCallableFacts *source = m->callable_facts;
    XrLirCallableFacts *copy = lir_callable_facts_allocate(source->function_count);
    if (!copy) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t f = 0; f < source->function_count; ++f) {
        if (!source->functions[f]) continue;
        XrLirCallableFunctionFacts view = source->functions[f]->view;
        LirCallableFunctionFacts *received = lir_callable_function_allocate(view.value_count, view.capture_count);
        if (!received) { xr_lir_callable_facts_destroy(copy); return XR_LIR_OUT_OF_MEMORY; }
        memcpy((void *)received->view.values, view.values, ((size_t)view.value_count + 1) * sizeof(*view.values));
        if (view.capture_count) memcpy((void *)received->view.captures, view.captures, (size_t)view.capture_count * sizeof(*view.captures));
        copy->functions[f] = received;
    }
    *empty = copy;
    return XR_LIR_OK;
}
XR_FUNC uint32_t xr_lir_callable_facts_function_count(const XrLirCallableFacts *facts) {
    return facts ? facts->function_count : 0;
}
XR_FUNC XrLirStatus xr_lir_callable_facts_function(const XrLirCallableFacts *facts, uint32_t function,
    XrLirCallableFunctionFacts *view) {
    if (!facts || !view || !function || function > facts->function_count) return XR_LIR_INVALID_ARGUMENT;
    *view = facts->functions[function - 1] ? facts->functions[function - 1]->view : (XrLirCallableFunctionFacts){0};
    return XR_LIR_OK;
}
