/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_state_verify.inc.c - Typed capability producers and SSA transport
 *
 * KEY CONCEPT:
 *   A matching tag alone cannot authorize a state access. The owned layout,
 *   exact reference shape and a grounded producer must agree.
 */
static bool lir_state_shape_equal(XrLirStateReference a, XrLirStateReference b) {
    return a.payload == b.payload && a.mutable == b.mutable;
}

static bool lir_state_signature(const XrLirModule *m, const XrLirFunctionData *f) {
    if (f->result == XR_LIR_STATE || f->result == XR_LIR_STATE_REF) return false;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        XrLirType type = f->parameter_types[p];
        if (type == XR_LIR_STATE_REF) return false;
        if (type == XR_LIR_STATE && (!m->state_layout_set ||
            f->kind != XR_LIR_FUNCTION_DEFINITION || p != f->parameter_count - 1)) return false;
    }
    return true;
}

static XrLirStatus lir_state_layout_verify(const XrLirModule *m, XrLirDiagnostic *d) {
    const XrLirStateLayout *layout = &m->state_layout;
    if (!m->state_layout_set) {
        if (m->state_entry || layout->modules || layout->module_count || layout->dependencies ||
            layout->dependency_count || layout->slots || layout->slot_count ||
            layout->payload_size || layout->payload_alignment)
            return xr_lir_internal_error(d, "absent state layout has residual storage", 0, 0, 0, NULL);
        return XR_LIR_OK;
    }
    if (!xr_lir_internal_state_layout_valid(layout) || !m->state_entry ||
        m->state_entry > m->function_count)
        return xr_lir_internal_error(d, "state layout or injected entry is invalid", 0, 0, 0, NULL);
    if (sizeof(uint32_t) > SIZE_MAX / layout->module_count) return XR_LIR_OUT_OF_MEMORY;
    uint8_t *colors = xr_calloc(layout->module_count, 1);
    uint32_t *stack = xr_malloc((size_t)layout->module_count * sizeof(*stack));
    uint32_t *next = xr_calloc(layout->module_count, sizeof(*next));
    if (!colors || !stack || !next) {
        xr_free(colors); xr_free(stack); xr_free(next);
        return XR_LIR_OUT_OF_MEMORY;
    }
    bool acyclic = true;
    for (uint32_t root = 0; root < layout->module_count && acyclic; ++root) {
        if (colors[root]) continue;
        uint32_t depth = 1;
        stack[0] = root;
        colors[root] = 1;
        while (depth && acyclic) {
            uint32_t current = stack[depth - 1];
            XrLirStateModule module = layout->modules[current];
            if (next[current] == module.dependency_count) { colors[current] = 2; --depth; continue; }
            uint32_t target = layout->dependencies[module.first_dependency + next[current]++];
            if (colors[target] == 1) { acyclic = false; break; }
            if (colors[target] == 2) continue;
            colors[target] = 1;
            stack[depth++] = target;
        }
    }
    xr_free(colors); xr_free(stack); xr_free(next);
    return acyclic ? XR_LIR_OK : xr_lir_internal_error(d, "module dependency cycle", 0, 0, 0, NULL);
}

static bool lir_state_instruction_types(const XrLirModule *m, const XrLirFunctionData *f,
    const XrLirInstructionView *v) {
    if (!m->state_layout_set) return false;
    XrLirType a = v->operand_count ? f->values[v->operands[0] - 1].type : XR_LIR_TYPE_INVALID;
    if (v->op == XR_LIR_MODULE_BEGIN || v->op == XR_LIR_MODULE_READY)
        return v->type == XR_LIR_UNIT && a == XR_LIR_STATE && v->bits < m->state_layout.module_count;
    if (v->op == XR_LIR_STATE_CONSTRUCT || v->op == XR_LIR_STATE_INITIALIZE ||
        v->op == XR_LIR_STATE_REFERENCE) {
        if (a != XR_LIR_STATE || v->bits >= m->state_layout.slot_count) return false;
        const XrLirStateSlot *slot = &m->state_layout.slots[v->bits];
        if (v->op != XR_LIR_STATE_REFERENCE &&
            v->operand_count != (slot->payload == XR_LIR_UNIT ? 1u : 2u)) return false;
        if (v->op == XR_LIR_STATE_INITIALIZE) {
            if (slot->mutable || v->type != XR_LIR_UNIT) return false;
        } else {
            if (v->type != XR_LIR_STATE_REF || (v->op == XR_LIR_STATE_CONSTRUCT && !slot->mutable)) return false;
            if (v->result > f->value_count || (v->result && !lir_state_shape_equal(f->values[v->result - 1].state_reference,
                (XrLirStateReference){slot->payload, slot->mutable}))) return false;
        }
        return v->op == XR_LIR_STATE_REFERENCE || slot->payload == XR_LIR_UNIT ||
            f->values[v->operands[1] - 1].type == slot->payload;
    }
    if (a != XR_LIR_STATE_REF || v->bits) return false;
    XrLirStateReference shape = f->values[v->operands[0] - 1].state_reference;
    if (shape.payload == XR_LIR_TYPE_INVALID) return false;
    if (v->op == XR_LIR_STATE_READ) return v->type == shape.payload;
    if (v->op == XR_LIR_STATE_PUBLISH) return v->type == XR_LIR_UNIT && shape.mutable;
    return v->op == XR_LIR_STATE_WRITE && v->type == XR_LIR_UNIT && shape.mutable &&
        v->operand_count == (shape.payload == XR_LIR_UNIT ? 1u : 2u) &&
        (shape.payload == XR_LIR_UNIT || f->values[v->operands[1] - 1].type == shape.payload);
}

/* Edges are indexed once. A queue propagates grounding, including backedges,
 * without rescanning the body for each block parameter or each iteration. */
typedef struct LirStateEdge { uint32_t target, next; } LirStateEdge;

static XrLirStatus lir_state_function(const XrLirModule *m, uint32_t index,
    XrLirDiagnostic *d) {
    const XrLirFunctionData *f = &m->functions[index];
    uint32_t capability_count = 0;
    for (uint32_t i = 0; i < f->value_count; ++i)
        capability_count += f->values[i].type == XR_LIR_STATE || f->values[i].type == XR_LIR_STATE_REF;
    if (!capability_count) return XR_LIR_OK;
    if (!m->state_entry || !f->parameter_count || f->parameter_types[f->parameter_count - 1] != XR_LIR_STATE)
        return xr_lir_internal_error(d, "stateful function lacks its trailing capability", index + 1, 0, 0, NULL);
    uint64_t edge_count = 0;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e)
            edge_count += term->edges[e].argument_count;
    }
    if (edge_count > UINT32_MAX || edge_count > SIZE_MAX / sizeof(LirStateEdge) ||
        sizeof(uint32_t) > SIZE_MAX / f->value_count) return XR_LIR_OUT_OF_MEMORY;
    uint32_t *heads = xr_malloc((size_t)f->value_count * sizeof(*heads));
    uint32_t *queue = xr_malloc((size_t)f->value_count * sizeof(*queue));
    uint8_t *grounded = xr_calloc(f->value_count, 1);
    LirStateEdge *edges = edge_count ? xr_malloc((size_t)edge_count * sizeof(*edges)) : NULL;
    if (!heads || !queue || !grounded || (edge_count && !edges)) {
        xr_free(heads); xr_free(queue); xr_free(grounded); xr_free(edges);
        return XR_LIR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < f->value_count; ++i) heads[i] = UINT32_MAX;
    uint32_t used = 0, count = 0;
    XrLirStatus status = XR_LIR_OK;
    for (uint32_t i = 0; i < f->value_count; ++i) {
        const XrLirValueData *v = &f->values[i];
        if (v->type != XR_LIR_STATE && v->type != XR_LIR_STATE_REF) continue;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, i + 1);
        if ((v->type == XR_LIR_STATE && v->parameter && !v->block) ||
            (v->type == XR_LIR_STATE_REF && definition &&
             (definition->op == XR_LIR_STATE_CONSTRUCT || definition->op == XR_LIR_STATE_REFERENCE))) {
            grounded[i] = 1;
            queue[count++] = i;
        }
    }
    for (uint32_t b = 0; b < f->block_count && status == XR_LIR_OK; ++b) {
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e) {
            const XrLirEdgeView *edge = &term->edges[e];
            const XrLirBlockData *target = &f->blocks[edge->target - 1];
            for (uint32_t a = 0; a < edge->argument_count; ++a) {
                uint32_t source = edge->arguments[a] - 1, parameter = target->parameters[a] - 1;
                XrLirType type = f->values[source].type;
                if (type != XR_LIR_STATE && type != XR_LIR_STATE_REF) continue;
                if (type == XR_LIR_STATE_REF && !lir_state_shape_equal(
                    f->values[source].state_reference, f->values[parameter].state_reference)) {
                    status = xr_lir_internal_error(d, "state reference edge shape mismatch", index + 1,
                        b + 1, f->blocks[b].instruction_count, &term->source);
                    break;
                }
                edges[used] = (LirStateEdge){parameter, heads[source]};
                heads[source] = used++;
            }
        }
    }
    for (uint32_t next = 0; next < count && status == XR_LIR_OK; ++next)
        for (uint32_t edge = heads[queue[next]]; edge != UINT32_MAX; edge = edges[edge].next) {
            uint32_t target = edges[edge].target;
            if (!grounded[target]) { grounded[target] = 1; queue[count++] = target; }
        }
    for (uint32_t i = 0; i < f->value_count && status == XR_LIR_OK; ++i)
        if ((f->values[i].type == XR_LIR_STATE || f->values[i].type == XR_LIR_STATE_REF) && !grounded[i])
            status = xr_lir_internal_error(d, "state capability has no verified producer", index + 1, 0, 0, NULL);
    xr_free(heads); xr_free(queue); xr_free(grounded); xr_free(edges);
    return status;
}
