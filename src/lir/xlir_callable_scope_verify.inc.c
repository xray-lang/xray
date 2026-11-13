/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_scope_verify.inc.c - Scope capability geometry on the SSA graph
 */
static bool lir_scope_contains(const uint32_t *parents, uint32_t count,
    uint32_t parent, uint32_t child) {
    for (uint64_t steps = 0; steps <= count && child <= count; ++steps) {
        if (parent == child) return true;
        if (!child) break;
        child = parents[child];
    }
    return false;
}
static bool lir_scope_callable(const XrLirModule *m, XrLirType type) {
    XrLirCallableType signature;
    XrLirCellType cell;
    if (xr_lir_cell_type_view(m, type, &cell) == XR_LIR_OK) return cell.payload == XR_LIR_I64;
    return type >= XR_LIR_TYPE_COUNT && xr_lir_callable_type_view(m, type, &signature) == XR_LIR_OK;
}
/* Frame storage belongs to the activation, while a captured canonical Cell
 * may retire earlier. Owner-changing operations preserve that borrow floor. */
static uint32_t lir_scope_frame_floor(const XrLirModule *m, const XrLirFunctionData *f,
    const uint32_t *parents, const uint32_t *owners, uint32_t value) {
    for (uint32_t steps = 0; steps < f->value_count; ++steps) {
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, value);
        if (!definition) return owners[value];
        if (definition->op == XR_LIR_FUNCTION_REF) {
            uint32_t floor = 0;
            if (definition->bits != XR_LIR_CLOSURE_FRAME) return floor;
            for (uint32_t p = 0; p < definition->operand_count; ++p) {
                uint32_t capture = definition->operands[p];
                XrLirCellType cell;
                if (xr_lir_cell_type_view(m, f->values[capture - 1].type, &cell) != XR_LIR_OK) continue;
                uint32_t owner = owners[capture];
                if (lir_scope_contains(parents, f->value_count, floor, owner)) floor = owner;
                else if (!lir_scope_contains(parents, f->value_count, owner, floor)) return UINT32_MAX;
            }
            return floor;
        }
        if (definition->op == XR_LIR_CALL || definition->op == XR_LIR_CALL_INDIRECT ||
            definition->op == XR_LIR_STATE_READ) return 0;
        if ((definition->op != XR_LIR_FUNCTION_BIND && definition->op != XR_LIR_FUNCTION_SCOPE_LEAVE) ||
            definition->operand_count != 2) return owners[value];
        value = definition->operands[1];
    }
    return UINT32_MAX;
}
static int lir_scope_binding_compare(const void *left, const void *right) {
    const uint32_t *a = left, *b = right;
    if (a[0] != b[0]) return a[0] < b[0] ? -1 : 1;
    return (a[1] > b[1]) - (a[1] < b[1]);
}
static XrLirStatus lir_callable_scopes(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, const LirGraph *graph, XrLirDiagnostic *diagnostic) {
    uint64_t values = (uint64_t)f->value_count + 1;
    uint64_t words = 4 * values + (uint64_t)f->block_count * 2;
    if (words > SIZE_MAX / sizeof(uint32_t)) return XR_LIR_OUT_OF_MEMORY;
    uint32_t *storage = xr_malloc((size_t)words * sizeof(*storage));
    if (!storage) return XR_LIR_OUT_OF_MEMORY;
    for (size_t n = 0; n < (size_t)words; ++n) storage[n] = UINT32_MAX;
    uint32_t *parents = storage, *owners = parents + (size_t)values;
    uint32_t *incoming = owners + (size_t)values, *outgoing = incoming + f->block_count;
    uint32_t *bindings = outgoing + f->block_count;
    size_t binding_count = 0;
    parents[0] = 0;
    bool captured_cells = false;
    for (uint32_t v = 0; v < f->value_count && !captured_cells; ++v) {
        XrLirCellType cell;
        captured_cells = xr_lir_cell_type_view(m, f->values[v].type, &cell) == XR_LIR_OK;
    }
    for (uint32_t p = 0; p < f->parameter_count; ++p)
        if (lir_scope_callable(m, f->parameter_types[p])) owners[f->parameters[p]] = 0;
    uint32_t bad_block = 0, bad_instruction = 0;
    const char *failure = "function scope parent or selected-edge lifetime is inconsistent";
    for (uint32_t order = 0; order < graph->count; ++order) {
        uint32_t b = graph->order[order];
        const XrLirBlockData *block = &f->blocks[b];
        bad_block = b + 1;
        uint32_t current = b == graph->entry ? 0 : UINT32_MAX;
        for (uint32_t p = graph->offsets[b]; p < graph->offsets[b + 1]; ++p) {
            uint32_t previous = outgoing[graph->predecessors[p]];
            if (previous == UINT32_MAX) continue;
            if (current != UINT32_MAX && current != previous) goto invalid;
            current = previous;
        }
        if (current == UINT32_MAX) goto invalid;
        incoming[b] = current;
        for (uint32_t p = 0; p < block->parameter_count; ++p) {
            uint32_t value = block->parameters[p];
            if (lir_scope_callable(m, f->values[value - 1].type)) owners[value] = current;
        }
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            const XrLirInstructionView *v = &block->instructions[n].view;
            bad_instruction = n + 1;
            if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER) {
                uint32_t parent = v->operand_count ? v->operands[0] : 0;
                if (parent != current) goto invalid;
                parents[v->result] = parent;
                current = v->result;
            } else if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) {
                if (v->operands[0] != current || parents[current] == UINT32_MAX) goto invalid;
                current = parents[current];
            } else if (v->op == XR_LIR_FUNCTION_BIND) {
                if (!lir_scope_contains(parents, f->value_count, v->operands[0], current)) goto invalid;
                bindings[binding_count * 2] = v->operands[0];
                bindings[binding_count++ * 2 + 1] = (uint32_t)v->bits;
            }
            if (captured_cells && (v->op == XR_LIR_FUNCTION_BIND || v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) && v->result) {
                XrLirCallableType signature;
                if (xr_lir_callable_type_view(m, v->type, &signature) == XR_LIR_OK) {
                    uint32_t receiving = v->op == XR_LIR_FUNCTION_BIND ? v->operands[0] : current;
                    uint32_t floor = lir_scope_frame_floor(m, f, parents, owners, v->operands[1]);
                    if (!lir_scope_contains(parents, f->value_count, floor, receiving)) goto invalid;
                }
            }
            if (v->result && lir_scope_callable(m, v->type))
                owners[v->result] = v->op == XR_LIR_FUNCTION_BIND ? v->operands[0] : current;
            if (v->op == XR_LIR_RET && current) goto invalid;
        }
        outgoing[b] = current;
    }
    qsort(bindings, binding_count, sizeof(*bindings) * 2, lir_scope_binding_compare);
    for (size_t n = 1; n < binding_count; ++n)
        if (!lir_scope_binding_compare(bindings + (n - 1) * 2, bindings + n * 2)) goto invalid;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        uint32_t current = incoming[b];
        bad_block = b + 1;
        for (uint32_t n = 0; n < block->instruction_count; ++n) {
            const XrLirInstructionView *v = &block->instructions[n].view;
            bad_instruction = n + 1;
            for (uint32_t p = 0; p < v->operand_count; ++p) {
                uint32_t value = v->operands[p];
                if (f->values[value - 1].type == XR_LIR_FUNCTION_SCOPE) {
                    const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, value);
                    if (!definition || definition->op != XR_LIR_FUNCTION_SCOPE_ENTER ||
                        !lir_scope_contains(parents, f->value_count, value, current)) goto invalid;
                } else if (lir_scope_callable(m, f->values[value - 1].type) &&
                    !lir_scope_contains(parents, f->value_count, owners[value], current)) goto invalid;
            }
            if (v->op == XR_LIR_FUNCTION_SCOPE_ENTER) current = v->result;
            if (v->op == XR_LIR_FUNCTION_SCOPE_LEAVE) current = parents[current];
            for (uint32_t e = 0; e < v->edge_count; ++e) {
                const XrLirEdgeView *edge = &v->edges[e];
                if (current != incoming[edge->target - 1]) goto invalid;
                for (uint32_t a = 0; a < edge->argument_count; ++a) {
                    uint32_t value = edge->arguments[a];
                    if (lir_scope_callable(m, f->values[value - 1].type) &&
                        !lir_scope_contains(parents, f->value_count, owners[value], current)) goto invalid;
                }
            }
        }
    }
    xr_free(storage);
    return XR_LIR_OK;
invalid:
    xr_free(storage);
    return xr_lir_internal_error(diagnostic, failure, function, bad_block, bad_instruction, NULL);
}
