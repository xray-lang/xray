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

static bool lir_callable_extent(size_t *bytes, size_t count, size_t stride) {
    if (stride && count > (SIZE_MAX - *bytes) / stride) return false;
    *bytes += count * stride;
    return true;
}

static XrLirCallableFacts *lir_callable_facts_allocate(uint32_t functions) {
    size_t bytes = sizeof(XrLirCallableFacts);
    if (!lir_callable_extent(&bytes, functions, sizeof(LirCallableFunctionFacts *))) return NULL;
    XrLirCallableFacts *facts = xr_calloc(1, bytes);
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
    size_t bytes = sizeof(LirCallableFunctionFacts);
    if (!rows || !lir_callable_extent(&bytes, rows, sizeof(XrLirCallableValueFact)) ||
        !lir_callable_extent(&bytes, captures, sizeof(uint32_t))) return NULL;
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
    uint32_t *links, uint32_t *path, uint8_t *colors) {
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t v = index + 1;
        if (!rows[v].type || !links[v]) continue;
        uint32_t current = v, depth = 0;
        while (links[current] && colors[current] != 1) {
            uint32_t next = links[current];
            if (!next || next > count || !rows[next].type || depth == count) return false;
            path[depth++] = current; colors[current] = 1; current = next;
        }
        bool persistent = colors[current] == 1 ? false : rows[current].persistent;
        uint32_t origin = colors[current] == 1 ? 0 : rows[current].origin;
        while (depth) {
            uint32_t value = path[--depth];
            rows[value].persistent = persistent; rows[value].origin = origin;
            rows[value].known_target = origin ? rows[origin].known_target : 0;
            links[value] = 0; colors[value] = 2;
        }
    }
    return true;
}

static void lir_callable_need_owner(XrLirCallableValueFact *rows, uint32_t value) {
    uint32_t origin = rows[value].origin;
    if (!origin) return;
    XrLirCallableValueFact *source = &rows[origin];
    if (source->operation == XR_LIR_FUNCTION_REF && source->constructor_storage == XR_LIR_CLOSURE_FRAME &&
        !source->capture_count && !source->state_capture) source->materialize = true;
}
/* These are checked consuming contracts, not the conservative persistence
 * predicate. Materializing at the exact constructor keeps every later borrow
 * unchanged and gives a code-only escape a real pending result owner. */
static void lir_callable_materializations(const XrLirModule *m, const XrLirFunctionData *f,
    XrLirCallableValueFact *rows) {
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            const XrLirInstructionView *v = &block->instructions[n].view;
            if (v->op == XR_LIR_RET && v->operand_count) lir_callable_need_owner(rows,v->operands[0]);
            else if ((v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_INITIALIZE || v->op == XR_LIR_STATE_WRITE) &&
                v->operand_count == 2) lir_callable_need_owner(rows,v->operands[1]);
            else if (v->op == XR_LIR_FUNCTION_REF && v->bits == XR_LIR_CLOSURE_OWNED) {
                for (uint32_t p = 0; p < v->operand_count; ++p) lir_callable_need_owner(rows,v->operands[p]);
            } else if (v->op == XR_LIR_CALL) {
                const XrLirFunctionData *target = &m->functions[v->callee - 1];
                for (uint32_t p = 0; p < target->closure.capture_count; ++p) lir_callable_need_owner(rows,v->operands[p]);
                for (uint32_t p = 0; p < target->callable_parameter_count; ++p)
                    if (target->callable_parameters[p].contract.escaping)
                        lir_callable_need_owner(rows,v->operands[target->callable_parameters[p].parameter]);
            } else if (v->op == XR_LIR_CALL_INDIRECT) {
                XrLirCallableType signature;
                XR_CHECK(xr_lir_callable_type_view(m,f->values[v->operands[0] - 1].type,&signature) == XR_LIR_OK,
                    "verified indirect calls retain their exact parameter contracts");
                for (uint32_t p = 0; p < signature.parameter_count; ++p)
                    if (signature.parameters[p].escaping) lir_callable_need_owner(rows,v->operands[p + 1]);
            }
        }
    }
}
typedef struct LirCallableFlowEdge { size_t next; uint32_t target; } LirCallableFlowEdge;
static void lir_callable_flow_edge(LirCallableFlowEdge *edges, size_t *used, size_t *heads,
    uint32_t source, uint32_t target) {
    edges[*used] = (LirCallableFlowEdge){heads[source],target}; heads[source] = (*used)++;
}
static XrLirStatus lir_callable_transports(const XrLirFunctionData *f, XrLirCallableValueFact *rows) {
    size_t count = (size_t)f->value_count + 1, edges = 0, bytes = 0;
    if (!count) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < f->value_count; ++i) edges += rows[i + 1].alias_source != 0;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *end = &f->blocks[b].instructions[f->blocks[b].instruction_count - 1].view;
        for (uint32_t e = 0; e < end->edge_count; ++e) {
            if (end->edges[e].argument_count > SIZE_MAX - edges) return XR_LIR_OUT_OF_MEMORY;
            edges += end->edges[e].argument_count;
        }
    }
    if (!lir_callable_extent(&bytes,edges,sizeof(LirCallableFlowEdge)) ||
        !lir_callable_extent(&bytes,count,sizeof(size_t) + sizeof(uint32_t) + 2)) return XR_LIR_OUT_OF_MEMORY;
    void *memory = xr_malloc(bytes);
    if (!memory) return XR_LIR_OUT_OF_MEMORY;
    LirCallableFlowEdge *flow = memory; size_t *heads = (size_t *)(flow + edges);
    uint32_t *work = (uint32_t *)(heads + count);
    uint8_t *masks = (uint8_t *)(work + count), *queued = masks + count;
    memset(masks,0,count * 2);
    size_t used = 0, waiting = 0;
    for (size_t v = 0; v < count; ++v) heads[v] = SIZE_MAX;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t v = i + 1;
        if (!rows[v].type || rows[v].type == XR_LIR_FUNCTION_SCOPE) continue;
        if (rows[v].parameter) masks[v] = 2;
        else if (rows[v].operation == XR_LIR_FUNCTION_REF)
            masks[v] = rows[v].materialize || rows[v].constructor_storage == XR_LIR_CLOSURE_OWNED ? 4 : 1;
        else if (rows[v].operation == XR_LIR_CALL || rows[v].operation == XR_LIR_CALL_INDIRECT ||
            rows[v].operation == XR_LIR_STATE_READ) masks[v] = 4;
        if (masks[v]) { work[waiting++] = v; queued[v] = 1; }
        if (rows[v].alias_source) lir_callable_flow_edge(flow,&used,heads,rows[v].alias_source,v);
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *end = &f->blocks[b].instructions[f->blocks[b].instruction_count - 1].view;
        for (uint32_t e = 0; e < end->edge_count; ++e) {
            const XrLirEdgeView *edge = &end->edges[e];
            const XrLirBlockData *target = &f->blocks[edge->target - 1];
            for (uint32_t p = 0; p < edge->argument_count; ++p)
                if (rows[target->parameters[p]].type && rows[target->parameters[p]].type != XR_LIR_FUNCTION_SCOPE)
                    lir_callable_flow_edge(flow,&used,heads,edge->arguments[p],target->parameters[p]);
        }
    }
    while (waiting) {
        uint32_t source = work[--waiting]; queued[source] = 0;
        for (size_t e = heads[source]; e != SIZE_MAX; e = flow[e].next) {
            uint32_t target = flow[e].target; uint8_t mask = masks[target] | masks[source];
            if (mask == masks[target]) continue;
            masks[target] = mask;
            if (!queued[target]) { work[waiting++] = target; queued[target] = 1; }
        }
    }
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t v = i + 1; uint8_t mask = masks[v];
        if (!rows[v].type || rows[v].type == XR_LIR_FUNCTION_SCOPE) continue;
        rows[v].transport = mask == 4 ? XR_LIR_CALLABLE_TRANSPORT_OWNED :
            mask == 1 ? XR_LIR_CALLABLE_TRANSPORT_FRAME :
            mask == 2 || mask == 3 ? XR_LIR_CALLABLE_TRANSPORT_BORROW : XR_LIR_CALLABLE_TRANSPORT_MIXED;
    }
    xr_free(memory);
    return XR_LIR_OK;
}

static XrLirStatus lir_callable_facts_capture(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, LirCallableGeometry geometry, XrLirCallableFacts **output) {
    if (!output || !lir_function_has_callables(m, f)) return XR_LIR_OK;
    uint32_t captures = 0;
    for (uint32_t index = 0; index < f->value_count; ++index) {
        uint32_t v = index + 1;
        const XrLirInstructionView *op = xr_lir_internal_value_definition(f, v);
        if (!op || op->op != XR_LIR_FUNCTION_REF) continue;
        if (op->operand_count > UINT32_MAX - captures) return XR_LIR_OUT_OF_MEMORY;
        captures += op->operand_count;
    }
    if (!*output) *output = lir_callable_facts_allocate(m->function_count);
    if (!*output) return XR_LIR_OUT_OF_MEMORY;
    LirCallableFunctionFacts *received = lir_callable_function_allocate(f->value_count, captures);
    size_t count = (size_t)f->value_count + 1;
    size_t stride = 2 * sizeof(uint32_t) + sizeof(uint8_t);
    uint32_t *scratch = count && count <= SIZE_MAX / stride ? xr_calloc(count, stride) : NULL;
    if (!received || !scratch) { xr_free(received); xr_free(scratch); return XR_LIR_OUT_OF_MEMORY; }
    XrLirCallableValueFact *rows = (XrLirCallableValueFact *)received->view.values;
    uint32_t *captured = (uint32_t *)received->view.captures, *links = scratch, *path = scratch + count;
    uint8_t *colors = (uint8_t *)(path + count);
    uint32_t contract = 0;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        rows[f->parameters[p]].parameter = p + 1;
        rows[f->parameters[p]].persistent = lir_callable_parameter_persistent(f, p, &contract);
    }
    uint32_t next_capture = 0;
    for (uint32_t index = 0; index < f->value_count; ++index) {
        uint32_t v = index + 1;
        XrLirCallableType signature;
        bool callable = xr_lir_callable_type_view(m, f->values[v - 1].type, &signature) == XR_LIR_OK;
        if (!callable && f->values[v - 1].type != XR_LIR_FUNCTION_SCOPE) {
            rows[v] = (XrLirCallableValueFact){0}; continue;
        }
        XrLirCallableValueFact *row = &rows[v];
        row->type = f->values[v - 1].type; row->origin = v; row->frame_floor = UINT32_MAX;
        row->block = f->values[v - 1].block;
        row->owner_scope = callable && geometry.owners ? geometry.owners[v] : 0;
        const XrLirInstructionView *op = xr_lir_internal_value_definition(f, v);
        if (op) {
            row->operation = op->op;
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
            row->persistent = lir_callable_persistence_seed(f, v, UINT64_MAX, &links[v]);
            row->alias_source = links[v];
        }
    }
    XR_DCHECK(next_capture == captures, "every constructor operand has one owned capture ordinal");
    bool resolved = lir_callable_facts_resolve(rows, f->value_count, links, path, colors);
    xr_free(scratch);
    if (!resolved) { xr_free(received); return XR_LIR_VERIFY_ERROR; }
    lir_callable_materializations(m,f,rows);
    XrLirStatus transport_status = lir_callable_transports(f,rows);
    if (transport_status != XR_LIR_OK) { xr_free(received); return transport_status; }
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
