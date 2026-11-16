/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_cell_verify.inc.c - Availability of lexical Cell owners and payload borrows
 */
typedef struct LirScopedCellFlow {
    const XrLirFunctionData *function;
    const LirGraph *graph;
    XrLirCallableFunctionFacts facts;
    uint32_t count, *indices, *values, *heads, *next;
    uint8_t *out, *current;
    uint64_t *dependencies;
    size_t words;
} LirScopedCellFlow;
static void lir_scoped_cells_free(LirScopedCellFlow *flow) {
    xr_free(flow->dependencies); xr_free(flow->current); xr_free(flow->out); xr_free(flow->indices);
}
static bool lir_scoped_cell_frame(const LirScopedCellFlow *flow, uint32_t value) {
    return flow->facts.values[value].kind == XR_LIR_SCOPED_VALUE_FUNCTION &&
        (flow->facts.values[value].transport == XR_LIR_CALLABLE_TRANSPORT_FRAME ||
         flow->facts.values[value].transport == XR_LIR_CALLABLE_TRANSPORT_BORROW);
}
/* A worklist propagates each captured Cell dependency through the existing
 * alias and selected-edge graph. Long alias chains do not rescan ancestors. */
static XrLirStatus lir_scoped_cell_dependencies(LirScopedCellFlow *flow) {
    const XrLirFunctionData *f = flow->function;
    size_t count = (size_t)f->value_count + 1, edge_count = 0, bytes = 0;
    bool needed = false;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t v = i + 1;
        const XrLirCallableValueFact *row = &flow->facts.values[v];
        if (!lir_scoped_cell_frame(flow,v)) continue;
        edge_count += row->alias_source != 0;
        for (uint32_t c = 0; c < row->capture_count; ++c)
            needed |= flow->indices[flow->facts.captures[row->first_capture + c]] != 0;
    }
    if (!needed) return XR_LIR_OK;
    flow->words = ((size_t)flow->count + 63) / 64;
    if (flow->words > SIZE_MAX / count || count * flow->words > SIZE_MAX / sizeof(uint64_t)) return XR_LIR_OUT_OF_MEMORY;
    flow->dependencies = xr_calloc(count * flow->words,sizeof(uint64_t));
    if (!flow->dependencies) return XR_LIR_OUT_OF_MEMORY;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *end = lir_terminator(f,b);
        for (uint32_t e = 0; e < end->edge_count; ++e) {
            if (end->edges[e].argument_count > SIZE_MAX - edge_count) return XR_LIR_OUT_OF_MEMORY;
            edge_count += end->edges[e].argument_count;
        }
    }
    if (!lir_callable_extent(&bytes,edge_count,sizeof(LirCallableFlowEdge)) ||
        !lir_callable_extent(&bytes,count,sizeof(size_t) + sizeof(uint32_t) + 1)) return XR_LIR_OUT_OF_MEMORY;
    void *memory = xr_malloc(bytes);
    if (!memory) return XR_LIR_OUT_OF_MEMORY;
    LirCallableFlowEdge *edges = memory; size_t *heads = (size_t *)(edges + edge_count);
    uint32_t *work = (uint32_t *)(heads + count); uint8_t *queued = (uint8_t *)(work + count);
    size_t used = 0, waiting = 0;
    memset(queued,0,count);
    for (size_t v = 0; v < count; ++v) heads[v] = SIZE_MAX;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        uint32_t v = i + 1;
        const XrLirCallableValueFact *row = &flow->facts.values[v];
        if (!lir_scoped_cell_frame(flow,v)) continue;
        if (row->alias_source) lir_callable_flow_edge(edges,&used,heads,row->alias_source,v);
        for (uint32_t c = 0; c < row->capture_count; ++c) {
            uint32_t index = flow->indices[flow->facts.captures[row->first_capture + c]];
            if (!index) continue;
            flow->dependencies[(size_t)v * flow->words + (index - 1) / 64] |= UINT64_C(1) << ((index - 1) % 64);
            if (!queued[v]) { queued[v] = 1; work[waiting++] = v; }
        }
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *end = lir_terminator(f,b);
        for (uint32_t e = 0; e < end->edge_count; ++e) {
            const XrLirEdgeView *edge = &end->edges[e];
            const XrLirBlockData *target = &f->blocks[edge->target - 1];
            for (uint32_t p = 0; p < edge->argument_count; ++p)
                if (lir_scoped_cell_frame(flow,target->parameters[p]))
                    lir_callable_flow_edge(edges,&used,heads,edge->arguments[p],target->parameters[p]);
        }
    }
    while (waiting) {
        uint32_t source = work[--waiting]; queued[source] = 0;
        for (size_t e = heads[source]; e != SIZE_MAX; e = edges[e].next) {
            uint32_t target = edges[e].target; bool changed = false;
            uint64_t *to = flow->dependencies + (size_t)target * flow->words;
            const uint64_t *from = flow->dependencies + (size_t)source * flow->words;
            for (size_t w = 0; w < flow->words; ++w) { uint64_t previous = to[w]; to[w] |= from[w]; changed |= previous != to[w]; }
            if (changed && !queued[target]) { queued[target] = 1; work[waiting++] = target; }
        }
    }
    xr_free(memory); return XR_LIR_OK;
}
static void lir_scoped_cell_input(LirScopedCellFlow *flow, uint32_t block) {
    const XrLirFunctionData *f = flow->function; const LirGraph *g = flow->graph;
    memset(flow->current,block == g->entry ? 1 : 0,flow->count);
    for (uint32_t p = g->offsets[block]; p < g->offsets[block + 1]; ++p) {
        const uint8_t *previous = flow->out + (size_t)g->predecessors[p] * flow->count;
        for (uint32_t c = 0; c < flow->count; ++c) flow->current[c] |= previous[c];
    }
    if (block == g->entry)
        for (uint32_t c = 0; c < flow->count; ++c)
            if (flow->facts.values[flow->values[c]].cell_transport == XR_LIR_CELL_TRANSPORT_BORROW) flow->current[c] = 2;
    const XrLirBlockData *body = &f->blocks[block];
    for (uint32_t p = 0; p < body->parameter_count; ++p) {
        uint32_t index = flow->indices[body->parameters[p]];
        if (!index) continue;
        uint8_t state = 0;
        for (uint32_t n = g->offsets[block]; n < g->offsets[block + 1]; ++n) {
            uint32_t previous = g->predecessors[n]; const XrLirInstructionView *end = lir_terminator(f,previous);
            for (uint32_t e = 0; e < end->edge_count; ++e) {
                if (end->edges[e].target != block + 1) continue;
                uint32_t source = flow->indices[end->edges[e].arguments[p]];
                XR_DCHECK(source,"a verified Cell block parameter receives an exact Cell identity");
                state |= flow->out[(size_t)previous * flow->count + source - 1];
            }
        }
        flow->current[index - 1] = state;
    }
}
static void lir_scoped_cell_transfer(LirScopedCellFlow *flow, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_VALUE_DROP && flow->indices[v->operands[0]]) flow->current[flow->indices[v->operands[0]] - 1] = 1;
    if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) {
        if (v->operand_count == 2) {
            uint32_t source = v->operands[1], index = flow->indices[source];
            if (index && flow->facts.values[source].cell_transport == XR_LIR_CELL_TRANSPORT_OWNED) flow->current[index - 1] = 1;
        }
        for (uint32_t index = flow->heads[v->operands[0]]; index; index = flow->next[index - 1]) flow->current[index - 1] = 1;
    }
    if (v->result && flow->indices[v->result]) flow->current[flow->indices[v->result] - 1] = 2;
}
static bool lir_scoped_cell_read(const LirScopedCellFlow *flow, uint32_t value) {
    uint32_t index = flow->indices[value];
    if (index) return flow->current[index - 1] == 2;
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(flow->function,value);
    if (definition && definition->op == XR_LIR_MAP_BORROW) {
        index = flow->indices[definition->operands[0]];
        if (index) return flow->current[index - 1] == 2;
    }
    if (flow->dependencies && lir_scoped_cell_frame(flow,value)) {
        const uint64_t *bits = flow->dependencies + (size_t)value * flow->words;
        for (uint32_t c = 0; c < flow->count; ++c)
            if ((bits[c / 64] & (UINT64_C(1) << (c % 64))) && flow->current[c] != 2) return false;
    }
    return true;
}
static XrLirStatus lir_scoped_cell_block(LirScopedCellFlow *flow, uint32_t function, uint32_t block, XrLirDiagnostic *d) {
    const XrLirBlockData *body = &flow->function->blocks[block];
    lir_scoped_cell_input(flow,block);
    for (uint32_t n = 0; n < body->instruction_count; ++n) {
        const XrLirInstructionView *v = &body->instructions[n].view;
        for (uint32_t p = 0; p < v->operand_count; ++p)
            if (!lir_scoped_cell_read(flow,v->operands[p]))
                return xr_lir_internal_error(d,"Cell use follows release or precedes ownership",function,block + 1,n + 1,&v->source);
        if (v->op == XR_LIR_VALUE_DROP && flow->indices[v->operands[0]] &&
            flow->facts.values[v->operands[0]].cell_transport == XR_LIR_CELL_TRANSPORT_BORROW)
            return xr_lir_internal_error(d,"borrowed Cell formal cannot be released",function,block + 1,n + 1,&v->source);
        if (v->op == XR_LIR_CELL_NEW && flow->indices[v->result] && flow->current[flow->indices[v->result] - 1] != 1)
            return xr_lir_internal_error(d,"Cell reconstruction requires an empty lexical lifetime",function,block + 1,n + 1,&v->source);
        for (uint32_t e = 0; e < v->edge_count; ++e)
            for (uint32_t p = 0; p < v->edges[e].argument_count; ++p)
                if (!lir_scoped_cell_read(flow,v->edges[e].arguments[p]))
                    return xr_lir_internal_error(d,"Cell edge follows release or precedes ownership",function,block + 1,n + 1,&v->source);
        lir_scoped_cell_transfer(flow,v);
    }
    return XR_LIR_OK;
}
static XrLirStatus lir_scoped_cells_verify(const XrLirModule *m, const XrLirFunctionData *f, uint32_t function,
    const LirGraph *graph, XrLirCallableFunctionFacts facts, XrLirDiagnostic *d) {
    LirScopedCellFlow flow = {.function=f,.graph=graph,.facts=facts};
    for (uint32_t v = 0; v < f->value_count; ++v) flow.count += xr_lir_cell_value_type(m,f->values[v].type,NULL);
    if (!flow.count) return XR_LIR_OK;
    size_t rows = (size_t)f->value_count + 1, words = 0, pairs = 0;
    if (!rows || !lir_callable_extent(&words,rows,2) || !lir_callable_extent(&words,flow.count,2) ||
        words > SIZE_MAX / sizeof(uint32_t) || !lir_callable_extent(&pairs,f->block_count,flow.count)) return XR_LIR_OUT_OF_MEMORY;
    flow.indices = xr_calloc(words,sizeof(uint32_t)); flow.out = xr_calloc(pairs,1); flow.current = xr_malloc(flow.count);
    if (!flow.indices || !flow.out || !flow.current) { lir_scoped_cells_free(&flow); return XR_LIR_OUT_OF_MEMORY; }
    flow.heads = flow.indices + rows; flow.values = flow.heads + rows; flow.next = flow.values + flow.count;
    uint32_t count = 0;
    for (uint32_t v = 0; v < f->value_count; ++v) {
        if (!xr_lir_cell_value_type(m,f->values[v].type,NULL)) continue;
        uint32_t index = ++count; flow.indices[v + 1] = index; flow.values[index - 1] = v + 1;
        if (facts.values[v + 1].cell_transport != XR_LIR_CELL_TRANSPORT_OWNED) continue;
        uint32_t scope = facts.values[v + 1].owner_scope;
        XR_DCHECK(scope <= f->value_count,"Cell owners consume the verifier's checked lexical geometry");
        flow.next[index - 1] = flow.heads[scope]; flow.heads[scope] = index;
    }
    XrLirStatus status = lir_scoped_cell_dependencies(&flow);
    bool changed = status == XR_LIR_OK;
    while (changed) {
        changed = false;
        for (uint32_t r = 0; r < graph->count; ++r) {
            uint32_t block = graph->order[r]; lir_scoped_cell_input(&flow,block);
            const XrLirBlockData *body = &f->blocks[block];
            for (uint32_t n = 0; n < body->instruction_count; ++n) lir_scoped_cell_transfer(&flow,&body->instructions[n].view);
            uint8_t *out = flow.out + (size_t)block * flow.count;
            if (memcmp(out,flow.current,flow.count)) { memcpy(out,flow.current,flow.count); changed = true; }
        }
    }
    for (uint32_t b = 0; b < f->block_count && status == XR_LIR_OK; ++b) status = lir_scoped_cell_block(&flow,function,b,d);
    lir_scoped_cells_free(&flow); return status;
}
