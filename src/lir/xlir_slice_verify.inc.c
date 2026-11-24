/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_slice_verify.inc.c - Logical loans independent of private keeper slots
 */
typedef struct LirSliceRoot { uint32_t kind, identity; } LirSliceRoot;
enum { LIR_SLICE_ROOT_LOCAL=1, LIR_SLICE_ROOT_FORMAL, LIR_SLICE_ROOT_STATE,
    LIR_SLICE_ROOT_UNKNOWN, LIR_SLICE_ROOT_STATE_ANY, LIR_SLICE_ROOT_INPUT };
typedef struct LirSliceFlow {
    uint32_t *values, *queue;
    uint8_t *inputs, *scratch, *edge, *queued;
    LirSliceRoot *bindings, *input_roots, *roots, *edge_roots;
    XrLirCallableFunctionFacts callables;
    uint32_t *callable_queue;
    uint8_t *callable_seen;
    size_t count, head, tail, pending;
    bool checked_inputs;
} LirSliceFlow;
static bool lir_slice_extent(uint64_t count, size_t stride) { return stride && count <= SIZE_MAX / stride; }
static void lir_slice_flow_drop(LirSliceFlow *flow) {
    xr_free(flow->values); xr_free(flow->queue); xr_free(flow->inputs);
    xr_free(flow->scratch); xr_free(flow->edge); xr_free(flow->queued);
    xr_free(flow->bindings); xr_free(flow->input_roots); xr_free(flow->roots); xr_free(flow->edge_roots);
    xr_free(flow->callable_queue); xr_free(flow->callable_seen);
}
static LirSliceRoot lir_slice_root_join(LirSliceRoot a, LirSliceRoot b) {
    if (!a.kind) return b;
    if (!b.kind || (a.kind == b.kind && a.identity == b.identity)) return a;
    return (LirSliceRoot){LIR_SLICE_ROOT_UNKNOWN,0};
}
static bool lir_slice_root_alias(LirSliceRoot a, LirSliceRoot b, bool checked_inputs) {
    if (!a.kind || !b.kind) return false;
    if (a.kind == LIR_SLICE_ROOT_UNKNOWN || b.kind == LIR_SLICE_ROOT_UNKNOWN) return true;
    if (a.kind == LIR_SLICE_ROOT_LOCAL || b.kind == LIR_SLICE_ROOT_LOCAL)
        return a.kind == b.kind && a.identity == b.identity;
    /* Completed call obligations separate borrowed inputs from State and
     * other input roots. Actual arguments are substituted at every call. */
    if (checked_inputs && (a.kind == LIR_SLICE_ROOT_INPUT || b.kind == LIR_SLICE_ROOT_INPUT))
        return a.kind == b.kind && a.identity == b.identity;
    return a.kind != LIR_SLICE_ROOT_STATE || b.kind != LIR_SLICE_ROOT_STATE || a.identity == b.identity;
}
static void lir_slice_binding_roots(const XrLirFunctionData *f, LirSliceRoot *roots) {
    for (uint32_t n = 0; n < f->value_count; ++n) {
        XrLirType type = f->values[n].type;
        if (type != XR_LIR_ARRAY_CELL && type != XR_LIR_ARRAY_REF && type != XR_LIR_STATE_REF) continue;
        if (type == XR_LIR_STATE_REF && f->values[n].state_reference.payload != XR_LIR_ARRAY) continue;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, n + 1);
        LirSliceRoot root = {type == XR_LIR_STATE_REF ? LIR_SLICE_ROOT_STATE_ANY : LIR_SLICE_ROOT_UNKNOWN,0};
        if (f->values[n].parameter && !f->values[n].block) root = (LirSliceRoot){LIR_SLICE_ROOT_FORMAL,n + 1};
        else if (definition && definition->op == XR_LIR_CELL_NEW) root = (LirSliceRoot){LIR_SLICE_ROOT_LOCAL,n + 1};
        else if (definition && (definition->op == XR_LIR_STATE_REFERENCE || definition->op == XR_LIR_STATE_CONSTRUCT))
            root = (LirSliceRoot){LIR_SLICE_ROOT_STATE,(uint32_t)definition->bits};
        else if (definition && definition->op == XR_LIR_ARRAY_BORROW && roots[definition->operands[0]].kind)
            root = roots[definition->operands[0]];
        roots[n + 1] = root;
    }
}
static XrLirStatus lir_slice_frontier(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, XrLirDiagnostic *d) {
    bool signature = xr_lir_internal_slice_type(m, f->result);
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        signature |= xr_lir_internal_slice_type(m, f->parameter_types[p]);
    XrLirSliceFunctionFact contract = {0};
    bool checked = xr_lir_internal_slice_function_fact(m, function, &contract) == XR_LIR_OK && contract.available;
    if (signature && (!checked || f->kind == XR_LIR_FUNCTION_EXTERNAL ||
        (xr_lir_internal_slice_type(m, f->result) && contract.result_parameter == UINT32_MAX))) {
        xr_lir_internal_error(d, "Slice parameters and returns require a checked caller loan", function, 0, 0, NULL);
        if (d) d->status = XR_LIR_UNSUPPORTED;
        return XR_LIR_UNSUPPORTED;
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            bool slice = xr_lir_internal_slice_type(m, v->type);
            for (uint32_t a = 0; a < v->operand_count; ++a)
                slice |= xr_lir_internal_slice_type(m, f->values[v->operands[a] - 1].type);
            XrLirSliceIntrinsic role;
            bool admitted = v->op == XR_LIR_CALL_INTRINSIC &&
                xr_lir_slice_intrinsic_view(m, v->intrinsic, &role) == XR_LIR_OK;
            if (v->op == XR_LIR_RET && checked && xr_lir_internal_slice_type(m, f->result)) admitted = true;
            if (v->op == XR_LIR_CALL || v->op == XR_LIR_INVOKE) {
                XrLirSliceFunctionFact callee;
                admitted = xr_lir_internal_slice_function_fact(m, v->callee, &callee) == XR_LIR_OK && callee.available &&
                    (!xr_lir_internal_slice_type(m, v->type) || callee.result_parameter < v->operand_count);
            }
            if (slice && !admitted) {
                xr_lir_internal_error(d, "Slice use requires a closed readonly loan operation", function, b + 1, i + 1, &v->source);
                if (d) d->status = XR_LIR_UNSUPPORTED;
                return XR_LIR_UNSUPPORTED;
            }
        }
    }
    return XR_LIR_OK;
}
static bool lir_slice_flow_create(const XrLirModule *m, const XrLirFunctionData *f, LirSliceFlow *flow) {
    if (!lir_slice_extent((uint64_t)f->value_count + 1, sizeof(*flow->values))) return false;
    size_t values = (size_t)f->value_count + 1;
    flow->values = xr_calloc(values, sizeof(*flow->values));
    if (!flow->values) return false;
    for (uint32_t n = 0; n < f->value_count; ++n)
        if (xr_lir_internal_slice_type(m, f->values[n].type)) flow->values[n + 1] = (uint32_t)++flow->count;
    if (!flow->count) return true;
    if (flow->callables.value_count) {
        if (flow->callables.value_count != f->value_count) return false;
        flow->callable_queue = xr_malloc(values * sizeof(*flow->callable_queue));
        flow->callable_seen = xr_calloc(values, 1);
        if (!flow->callable_queue || !flow->callable_seen) return false;
    }
    if (!lir_slice_extent(f->block_count, flow->count) ||
        !lir_slice_extent(f->block_count, sizeof(*flow->queue))) return false;
    flow->inputs = xr_calloc(f->block_count, flow->count);
    flow->scratch = xr_malloc(flow->count); flow->edge = xr_malloc(flow->count);
    flow->queued = xr_calloc(f->block_count, 1);
    flow->queue = xr_malloc((size_t)f->block_count * sizeof(*flow->queue));
    if (!lir_slice_extent(values, sizeof(*flow->bindings)) ||
        !lir_slice_extent((uint64_t)f->block_count * flow->count, sizeof(*flow->input_roots)) ||
        !lir_slice_extent(flow->count, sizeof(*flow->roots))) return false;
    flow->bindings = xr_calloc(values, sizeof(*flow->bindings));
    flow->input_roots = xr_calloc((size_t)f->block_count * flow->count, sizeof(*flow->input_roots));
    flow->roots = xr_malloc(flow->count * sizeof(*flow->roots));
    flow->edge_roots = xr_malloc(flow->count * sizeof(*flow->edge_roots));
    if (flow->bindings) lir_slice_binding_roots(f, flow->bindings);
    return flow->inputs && flow->scratch && flow->edge && flow->queued && flow->queue &&
        flow->bindings && flow->input_roots && flow->roots && flow->edge_roots;
}
static void lir_slice_enqueue(LirSliceFlow *flow, uint32_t block, uint32_t blocks) {
    if (flow->queued[block]) return;
    flow->queue[flow->tail] = block; flow->tail = (flow->tail + 1) % blocks;
    ++flow->pending; flow->queued[block] = 1;
}
static bool lir_slice_write_conflict(const LirSliceFlow *flow, LirSliceRoot root) {
    for (size_t n = 0; n < flow->count; ++n)
        if ((flow->scratch[n] & 2u) && lir_slice_root_alias(flow->roots[n], root, flow->checked_inputs)) return true;
    return false;
}
static bool lir_slice_callable_frontier(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v, const LirSliceFlow *flow) {
    if (v->op != XR_LIR_CALL && v->op != XR_LIR_CALL_INDIRECT) return false;
    bool has_live_slice = false;
    for (size_t n = 0; n < flow->count; ++n) has_live_slice |= (flow->scratch[n] & 2u) != 0;
    if (!has_live_slice) return false;
    if (v->op == XR_LIR_CALL_INDIRECT) return true;
    for (uint32_t a = 0; a < v->operand_count; ++a) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(m, f->values[v->operands[a] - 1].type, &signature) == XR_LIR_OK) return true;
    }
    return false;
}
static bool lir_slice_call_conflict(const XrLirModule *m, const XrLirInstructionView *v, const LirSliceFlow *flow) {
    if (m->functions[v->callee - 1].kind == XR_LIR_FUNCTION_EXTERNAL) {
        XrLirExternKind kind = m->functions[v->callee - 1].extern_kind;
        if (kind == XR_LIR_EXTERN_PRINT_I64 || kind == XR_LIR_EXTERN_PRINT_STRING ||
            (kind >= XR_LIR_EXTERN_CLOCK_REALTIME && kind <= XR_LIR_EXTERN_UTC_OFFSET_AT)) return false;
    }
    XrLirSliceFunctionFact callee; XrLirSliceFactsView facts;
    if (xr_lir_internal_slice_function_fact(m, v->callee, &callee) != XR_LIR_OK || !callee.available ||
        xr_lir_slice_facts_view(m->slice_facts, &facts) != XR_LIR_OK) {
        /* No summary is not an empty WRITE set, including a zero-arg call. */
        return lir_slice_write_conflict(flow, (LirSliceRoot){LIR_SLICE_ROOT_UNKNOWN,0});
    }
    for (size_t n = 0; n < callee.write.count; ++n) {
        XrLirSliceEffect term = facts.terms[callee.write.first + n];
        LirSliceRoot root = {LIR_SLICE_ROOT_UNKNOWN,0};
        if (term.kind == XR_LIR_SLICE_EFFECT_SLOT) root = (LirSliceRoot){LIR_SLICE_ROOT_STATE,term.identity};
        else if (term.kind == XR_LIR_SLICE_EFFECT_PARAMETER || term.kind == XR_LIR_SLICE_EFFECT_CAPTURE) {
            if (term.identity >= v->operand_count) return true;
            uint32_t actual = v->operands[term.identity], slice = flow->values[actual];
            root = slice ? flow->roots[slice - 1] : flow->bindings[actual];
            if (!root.kind) root = (LirSliceRoot){LIR_SLICE_ROOT_UNKNOWN,0};
        }
        if (lir_slice_write_conflict(flow, root)) return true;
    }
    return false;
}
#include "xlir_slice_callable.inc.c"
static bool lir_slice_instruction_conflict(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v, const LirSliceFlow *flow) {
    if (v->op == XR_LIR_INVOKE || (v->op == XR_LIR_CALL && m->slice_facts))
        return lir_slice_call_conflict(m, v, flow);
    if (v->op == XR_LIR_CALL || v->op == XR_LIR_CALL_INDIRECT || v->op == XR_LIR_FUNCTION_REF) {
        for (uint32_t a = 0; a < v->operand_count; ++a) {
            uint32_t value = v->operands[a];
            LirSliceRoot root = f->values[value - 1].type == XR_LIR_STATE ?
                (LirSliceRoot){LIR_SLICE_ROOT_STATE_ANY,0} : flow->bindings[value];
            if (lir_slice_write_conflict(flow, root)) return true;
        }
        return false;
    }
    if (v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_INITIALIZE) {
        LirSliceRoot root = {LIR_SLICE_ROOT_STATE,(uint32_t)v->bits};
        return m->state_layout.slots[v->bits].payload == XR_LIR_ARRAY && lir_slice_write_conflict(flow, root);
    }
    bool write = v->op == XR_LIR_CELL_WRITE || v->op == XR_LIR_ARRAY_REF_WRITE ||
        v->op == XR_LIR_ARRAY_CELL_DROP || v->op == XR_LIR_STATE_WRITE || v->op == XR_LIR_STATE_PUBLISH;
    const XrLirArrayIntrinsic *array = v->op == XR_LIR_CALL_INTRINSIC ? xr_lir_internal_array_intrinsic(m, v->intrinsic) : NULL;
    write |= array && (array->operation == XR_LIR_ARRAY_OP_SET || array->operation == XR_LIR_ARRAY_OP_PUSH);
    return write && v->operand_count && lir_slice_write_conflict(flow, flow->bindings[v->operands[0]]);
}
static XrLirStatus lir_slice_block(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, uint32_t b, LirSliceFlow *flow, bool check, XrLirDiagnostic *d) {
    memcpy(flow->scratch, flow->inputs + (size_t)b * flow->count, flow->count);
    memcpy(flow->roots, flow->input_roots + (size_t)b * flow->count, flow->count * sizeof(*flow->roots));
    const XrLirBlockData *block = &f->blocks[b];
    for (uint32_t i = 0; i < block->instruction_count; ++i) {
        const XrLirInstructionView *v = &block->instructions[i].view;
        if (v->op == XR_LIR_SLICE_CLEANUP || v->op == XR_LIR_SLICE_FORMAL_CLEANUP) {
            flow->scratch[flow->values[(uint32_t)v->bits] - 1] = 1;
            continue;
        }
        bool callable_checked = check && lir_slice_callable_frontier(m, f, v, flow);
        LirSliceCallableCheck callable = callable_checked ? lir_slice_callable_check(m, f, function, v, flow) : LIR_SLICE_CALLABLE_SAFE;
        if (callable_checked && callable == LIR_SLICE_CALLABLE_UNKNOWN) {
            const char *message = v->op == XR_LIR_CALL_INDIRECT ?
                "indirect call with a live Slice requires captured binding effect facts" :
                "callable arguments with a live Slice require captured binding effect facts";
            xr_lir_internal_error(d, message, function, b + 1, i + 1, &v->source);
            if (d) d->status = XR_LIR_UNSUPPORTED;
            return XR_LIR_UNSUPPORTED;
        }
        if (check && (callable == LIR_SLICE_CALLABLE_CONFLICT ||
            (!callable_checked && lir_slice_instruction_conflict(m, f, v, flow))))
            return xr_lir_internal_error(d, "Array binding write conflicts with a live readonly Slice loan", function, b + 1, i + 1, &v->source);
        for (uint32_t a = 0; check && a < v->operand_count; ++a) {
            uint32_t row = flow->values[v->operands[a]];
            if (row && flow->scratch[row - 1] != 2)
                return xr_lir_internal_error(d, "Slice use is outside its live loan", function, b + 1, i + 1, &v->source);
        }
        if (v->op == XR_LIR_CALL && v->result && flow->values[v->result]) {
            XrLirSliceFunctionFact callee;
            if (xr_lir_internal_slice_function_fact(m, v->callee, &callee) != XR_LIR_OK ||
                !callee.available || callee.result_parameter >= v->operand_count) return XR_LIR_UNSUPPORTED;
            uint32_t source = flow->values[v->operands[callee.result_parameter]];
            if (!source) return XR_LIR_VERIFY_ERROR;
            uint32_t row = flow->values[v->result] - 1;
            flow->scratch[row] = 2; flow->roots[row] = flow->roots[source - 1];
        }
        if (check && v->op == XR_LIR_RET && v->operand_count && flow->values[v->operands[0]]) {
            XrLirSliceFunctionFact own;
            uint32_t row = flow->values[v->operands[0]] - 1;
            if (xr_lir_internal_slice_function_fact(m, function, &own) != XR_LIR_OK || !own.available ||
                own.result_parameter >= f->parameter_count || flow->roots[row].kind != LIR_SLICE_ROOT_INPUT ||
                flow->roots[row].identity != f->parameters[own.result_parameter])
                return xr_lir_internal_error(d, "Slice return is not the verified borrowed input origin", function, b + 1, i + 1, &v->source);
        }
        XrLirSliceIntrinsic role;
        if (v->op != XR_LIR_CALL_INTRINSIC || xr_lir_slice_intrinsic_view(m, v->intrinsic, &role) != XR_LIR_OK) continue;
        if (role.operation == XR_LIR_SLICE_OP_END) flow->scratch[flow->values[v->operands[0]] - 1] = 1;
        else if (v->result && flow->values[v->result]) {
            uint32_t row = flow->values[v->result] - 1;
            flow->scratch[row] = 2;
            flow->roots[row] = role.operation == XR_LIR_SLICE_OP_NEW ? flow->bindings[v->operands[0]] :
                flow->roots[flow->values[v->operands[0]] - 1];
        }
    }
    const XrLirInstructionView *term = lir_terminator(f, b);
    for (uint32_t e = 0; check && e < term->edge_count; ++e)
        for (uint32_t a = 0; a < term->edges[e].argument_count; ++a) {
            uint32_t row = flow->values[term->edges[e].arguments[a]];
            if (row && flow->scratch[row - 1] != 2)
                return xr_lir_internal_error(d, "selected edge requires a live Slice source", function, b + 1, block->instruction_count, &term->source);
        }
    return XR_LIR_OK;
}
static void lir_slice_successors(const XrLirFunctionData *f, uint32_t b, LirSliceFlow *flow) {
    const XrLirInstructionView *term = lir_terminator(f, b);
    for (uint32_t e = 0; e < term->edge_count; ++e) {
        const XrLirEdgeView *edge = &term->edges[e];
        const XrLirBlockData *target = &f->blocks[edge->target - 1];
        memcpy(flow->edge, flow->scratch, flow->count);
        memcpy(flow->edge_roots, flow->roots, flow->count * sizeof(*flow->edge_roots));
        for (uint32_t a = 0; a < edge->argument_count; ++a) {
            uint32_t row = flow->values[target->parameters[a]];
            if (row) {
                flow->edge[row - 1] = flow->scratch[flow->values[edge->arguments[a]] - 1];
                flow->edge_roots[row - 1] = flow->roots[flow->values[edge->arguments[a]] - 1];
            }
        }
        uint8_t *input = flow->inputs + (size_t)(edge->target - 1) * flow->count;
        LirSliceRoot *roots = flow->input_roots + (size_t)(edge->target - 1) * flow->count;
        bool changed = false;
        for (size_t n = 0; n < flow->count; ++n) {
            uint8_t joined = input[n] | flow->edge[n];
            changed |= joined != input[n]; input[n] = joined;
            LirSliceRoot root = lir_slice_root_join(roots[n], flow->edge_roots[n]);
            changed |= root.kind != roots[n].kind || root.identity != roots[n].identity; roots[n] = root;
        }
        if (changed) lir_slice_enqueue(flow, edge->target - 1, f->block_count);
    }
}
static XrLirStatus lir_slice_function(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, const XrLirCallableFacts *callables, XrLirDiagnostic *d) {
    XrLirStatus status = lir_slice_frontier(m, f, function, d);
    if (status != XR_LIR_OK || f->kind == XR_LIR_FUNCTION_EXTERNAL) return status;
    bool needed = false;
    for (uint32_t n = 0; n < f->value_count; ++n) needed |= xr_lir_internal_slice_type(m, f->values[n].type);
    if (!needed) return XR_LIR_OK;
    LirSliceFlow flow = {0};
    if (callables && xr_lir_callable_facts_function(callables,function,&flow.callables) != XR_LIR_OK)
        return XR_LIR_VERIFY_ERROR;
    if (!lir_slice_flow_create(m, f, &flow)) { lir_slice_flow_drop(&flow); return XR_LIR_OUT_OF_MEMORY; }
    XrLirSliceFunctionFact contract;
    flow.checked_inputs = xr_lir_internal_slice_function_fact(m, function, &contract) == XR_LIR_OK && contract.available;
    memset(flow.inputs + (size_t)(f->entry - 1) * flow.count, 1, flow.count);
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        uint32_t value = f->parameters[p], row = flow.values[value];
        if (!row) continue;
        flow.inputs[(size_t)(f->entry - 1) * flow.count + row - 1] = 2;
        flow.input_roots[(size_t)(f->entry - 1) * flow.count + row - 1] = (LirSliceRoot){LIR_SLICE_ROOT_INPUT,value};
    }
    lir_slice_enqueue(&flow, f->entry - 1, f->block_count);
    while (flow.pending) {
        uint32_t b = flow.queue[flow.head]; flow.head = (flow.head + 1) % f->block_count;
        --flow.pending; flow.queued[b] = 0;
        (void)lir_slice_block(m, f, function, b, &flow, false, NULL);
        lir_slice_successors(f, b, &flow);
    }
    for (uint32_t b = 0; b < f->block_count && status == XR_LIR_OK; ++b)
        status = lir_slice_block(m, f, function, b, &flow, true, d);
    lir_slice_flow_drop(&flow);
    return status;
}
